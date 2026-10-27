/* SPDX-License-Identifier: GPL-3.0-only */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "host_internal.h"
#include "lr2021_sim.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dirent.h>
#include <glob.h>
#endif

#define RING_BLOCKS 4096u           /* about 2.6 M pairs, 21 s at 125 ksample/s */
#define BLOCK_PAIRS LR2021_BRIDGE_RX_MAX_PAIRS
typedef struct {
    uint32_t generation, rate_hz, pairs;
    uint64_t first_pair, start_time;
    int16_t iq[2*BLOCK_PAIRS];
} block;
struct lr2021_host {
    int backend; lrh_fd fd;
    unsigned timeout_ms, baud, format;
    lrh_thread reader; bool reader_running;
    lrh_mutex mu, req_mu;
    lrh_cond cv;
    bool link_down, closing;
    uint16_t seq;
    bool resp_ready; uint8_t resp_type; uint16_t resp_seq, resp_len;
    uint8_t resp[LR2021_BRIDGE_MAX_PAYLOAD];
    lr2021_bridge_hello hello;
    block *ring; size_t head, tail, offset;
    bool host_overflow;
    uint32_t generation, rate_hz; bool streaming, ended;
    uint64_t expect_pair; bool expect_valid;
    lr2021_bridge_rx_end end;
    bool tx_done; lr2021_bridge_tx_end tx_end;
    lr2021_bridge_parser parser;
    /* in-process server */
    lrh_fd server_fd; lrh_server *server;
    lr2021_sim *sim; lr2021_iq_dev *dev; void *bus;
};

void lr2021_host_args_init(lr2021_host_args *a)
{
    memset(a,0,sizeof(*a));
    a->baud=LR2021_HOST_DEFAULT_BAUD; a->boot_ms=1200; a->port=LR2021_BRIDGE_TCP_PORT;
    a->reset_line=a->busy_line=-1; a->spi_hz=16000000; a->tcxo_ticks=32768;
    a->sim_pattern=-1; a->sim_tone_hz=0; a->timeout_ms=2000;
}
static int number(const char *v, unsigned long max, unsigned long *out)
{
    char *end; errno=0;
    unsigned long n=strtoul(v,&end,0);
    if(!*v || *end || errno || n>max)return -1;
    *out=n; return 0;
}
static int copy(char *dst, size_t n, const char *v)
{ if(strlen(v)>=n)return -1; strcpy(dst,v); return 0; }
int lr2021_host_args_set(lr2021_host_args *a, const char *k, const char *v)
{
    unsigned long n;
    if(!strcmp(k,"serial"))return copy(a->serial,sizeof(a->serial),v);
    if(!strcmp(k,"host"))return copy(a->host,sizeof(a->host),v);
    if(!strcmp(k,"spidev"))return copy(a->spidev,sizeof(a->spidev),v);
    if(!strcmp(k,"gpiochip"))return copy(a->gpiochip,sizeof(a->gpiochip),v);
    if(!strcmp(k,"tcxo"))return copy(a->tcxo,sizeof(a->tcxo),v);
    if(!strcmp(k,"rfsw"))return copy(a->rfsw,sizeof(a->rfsw),v);
    if(!strcmp(k,"format")) {
        if(!strcmp(v,"cs16")){ a->format=LR2021_BRIDGE_CS16; return 0; }
        if(!strcmp(v,"cs12")){ a->format=LR2021_BRIDGE_CS12; return 0; }
        return -1;
    }
    if(!strcmp(k,"sim")) {
        if(!strcmp(v,"counter") || !strcmp(v,"1")){ a->sim_pattern=LR2021_SIM_COUNTER; return 0; }
        if(!strcmp(v,"tone")){ a->sim_pattern=LR2021_SIM_TONE; return 0; }
        return -1;
    }
    unsigned *u=NULL; unsigned long max=UINT_MAX;
    if(!strcmp(k,"baud"))u=&a->baud;
    else if(!strcmp(k,"boot_ms")){ u=&a->boot_ms; max=10000; }
    else if(!strcmp(k,"port")){ u=&a->port; max=65535; }
    else if(!strcmp(k,"spi_hz")){ u=&a->spi_hz; max=16000000; }
    else if(!strcmp(k,"tcxo_ticks"))u=&a->tcxo_ticks;
    else if(!strcmp(k,"timeout_ms")){ u=&a->timeout_ms; max=60000; }
    else if(!strcmp(k,"tone")){ if(number(v,UINT32_MAX,&n))return -1; a->sim_tone_hz=(uint32_t)n; return 0; }
    else if(!strcmp(k,"reset") || !strcmp(k,"busy")) {
        if(number(v,1023,&n))return -1;
        *(!strcmp(k,"reset")?&a->reset_line:&a->busy_line)=(int)n; return 0;
    }
    else return 1;
    if(number(v,max,&n))return -1;
    *u=(unsigned)n; return 0;
}

const char *lr2021_host_strerror(int e)
{
    switch(e) {
    case LR2021_HOST_ETIMEOUT: return "no response from device";
    case LR2021_HOST_ELINK: return "link closed";
    case LR2021_HOST_EPROTO: return "unexpected response";
    case LR2021_HOST_EARG: return "invalid argument";
    default: return lr2021_bridge_strerror(e);
    }
}

/* Reader thread: responses, RX blocks, RX_END and TX_END. */
static void on_frame(void *ctx, const lr2021_bridge_frame *f)
{
    lr2021_host *h=ctx;
    if(f->type==LR2021_BRIDGE_RX_DATA) {
        lr2021_bridge_rx_data d;
        if(lr2021_bridge_unpack_rx_data(f->payload,f->length,&d) || d.generation!=h->generation)return;
        if(h->head-h->tail>=RING_BLOCKS){ h->host_overflow=true; return; }
        block *b=&h->ring[h->head%RING_BLOCKS];
        b->generation=d.generation; b->rate_hz=d.rate_hz; b->first_pair=d.first_pair;
        b->start_time=d.start_time; b->pairs=d.pairs;
        if(lr2021_bridge_unpack_samples(b->iq,d.format,d.shift,f->payload+LR2021_BRIDGE_RX_DATA_HEADER,
            f->length-LR2021_BRIDGE_RX_DATA_HEADER,d.pairs))return;
        h->head++;
    } else if(f->type==LR2021_BRIDGE_RX_END) {
        lr2021_bridge_rx_end e;
        if(lr2021_bridge_unpack_rx_end(f->payload,f->length,&e) || e.generation!=h->generation)return;
        h->end=e; h->ended=true; h->streaming=false;
    } else if(f->type==LR2021_BRIDGE_TX_END) {
        if(!lr2021_bridge_unpack_tx_end(f->payload,f->length,&h->tx_end))h->tx_done=true;
    } else if((f->type&0x80) && f->type<0xC0) {
        h->resp_ready=true; h->resp_type=f->type; h->resp_seq=f->sequence; h->resp_len=f->length;
        memcpy(h->resp,f->payload,f->length);
        /* RX data can follow the RX_START response in the same read: accept
         * the new generation here rather than after the requester wakes. */
        if(f->type==(LR2021_BRIDGE_RX_START|0x80) && f->length>=12 && !lr2021_bridge_get32(f->payload)) {
            h->generation=lr2021_bridge_get32(f->payload+4); h->rate_hz=lr2021_bridge_get32(f->payload+8);
            h->streaming=true; h->ended=false;
        }
    } else return;
    lrh_cond_broadcast(&h->cv);
}
/* A serial read returns at least every 100 ms, so the reader sees closing
 * without being cancelled. Sockets end on shutdown. */
static void *reader(void *c)
{
    lr2021_host *h=c;
    uint8_t buf[8192];
    bool serial=h->backend==LR2021_HOST_SERIAL;
    for(;;) {
        long n=lrh_read(h->fd,buf,sizeof(buf));
        lrh_lock(&h->mu);
        bool closing=h->closing;
        if(n>0)lr2021_bridge_parse(&h->parser,buf,(size_t)n,on_frame,h);
        lrh_unlock(&h->mu);
        if(closing)break;
        if(n==LRH_AGAIN || (n==0 && serial))continue; /* VTIME expiry */
        if(n<=0)break;
    }
    lrh_lock(&h->mu); h->link_down=true; lrh_cond_broadcast(&h->cv); lrh_unlock(&h->mu);
    return NULL;
}
static uint64_t deadline(unsigned ms) { return lrh_now_us()+(uint64_t)ms*1000u; }
/* Sends a request and waits for its response; copies up to cap bytes after
 * the status. Returns the device status or a host error. */
static int request(lr2021_host *h, uint8_t type, const uint8_t *p, size_t n,
                   uint8_t *out, size_t cap, size_t *out_len, unsigned timeout_ms)
{
    uint8_t frame[LR2021_BRIDGE_MAX_FRAME];
    lrh_lock(&h->req_mu);
    lrh_lock(&h->mu);
    uint16_t seq=++h->seq; h->resp_ready=false;
    bool down=h->link_down;
    lrh_unlock(&h->mu);
    int e=down?LR2021_HOST_ELINK:0;
    size_t len=lr2021_bridge_encode(frame,sizeof(frame),type,seq,p,n);
    if(!e && (!len || lrh_write_all(h->fd,frame,len)))e=LR2021_HOST_ELINK;
    if(!e) {
        uint64_t t=deadline(timeout_ms);
        lrh_lock(&h->mu);
        while(!(h->resp_ready && h->resp_seq==seq && h->resp_type==(type|0x80)) && !h->link_down) {
            h->resp_ready=h->resp_ready && h->resp_seq==seq;
            if(!lrh_cond_wait_until(&h->cv,&h->mu,t))break;
        }
        if(h->resp_ready && h->resp_seq==seq && h->resp_type==(type|0x80)) {
            if(h->resp_len<4)e=LR2021_HOST_EPROTO;
            else {
                e=(int32_t)lr2021_bridge_get32(h->resp);
                size_t m=(size_t)h->resp_len-4u<cap?(size_t)h->resp_len-4u:cap;
                if(out && m)memcpy(out,h->resp+4,m);
                if(out_len)*out_len=h->resp_len-4u;
            }
            h->resp_ready=false;
        } else e=h->link_down?LR2021_HOST_ELINK:LR2021_HOST_ETIMEOUT;
        lrh_unlock(&h->mu);
    }
    lrh_unlock(&h->req_mu);
    return e;
}
static int hello(lr2021_host *h, unsigned timeout_ms)
{
    uint8_t p[2]={1,0}, r[96]; size_t n=0;
    int e=request(h,LR2021_BRIDGE_HELLO,p,2,r,sizeof(r),&n,timeout_ms);
    if(e)return e;
    uint8_t full[84]; lr2021_bridge_put32(full,0); memcpy(full+4,r,n<80?n:80);
    if(n!=80 || lr2021_bridge_unpack_hello(full,84,&h->hello))return LR2021_HOST_EPROTO;
    return h->hello.version==LR2021_BRIDGE_VERSION ? 0 : LR2021_BRIDGE_EUNSUPPORTED;
}
static int serial_enter(lr2021_host *h)
{
    if(!hello(h,400))return 0;
    for(unsigned attempt=0;attempt<2;attempt++) {
        char text[48];
        int n=snprintf(text,sizeof(text),"\r\nbridge %u\r\n",h->baud);
        if(lrh_serial_baud(h->fd,115200))return LR2021_HOST_ELINK;
        lrh_write_all(h->fd,(const uint8_t *)text,(size_t)n);
        lrh_sleep_us(150000);
        if(lrh_serial_baud(h->fd,h->baud))return LR2021_HOST_ELINK;
        lrh_serial_flush(h->fd);
        if(!hello(h,1000))return 0;
    }
    return LR2021_HOST_ETIMEOUT;
}
static int sim_configure(void *c, const lr2021_iq_rx *rx, uint16_t *errors)
{
    lr2021_iq_dev *d=c;
    int e=lr2021_iq_dev_configure(d,rx); *errors=d->errors; return e;
}
static uint64_t sim_clock(void *c) { (void)c; return lrh_now_us(); }
static void sim_sleep(void *c, unsigned us) { (void)c; lrh_sleep_us(us); }
static int start_local(lr2021_host *h, const lr2021_host_args *a, char *err, size_t n)
{
    lr2021_iq_bus bus;
    lr2021_iq_board board={0};
    if(h->backend==LR2021_HOST_SIM) {
        if(!(h->sim=calloc(1,sizeof(*h->sim))))return -1;
        lr2021_sim_init(h->sim);
        h->sim->clock=sim_clock; h->sim->sleep=sim_sleep; h->sim->stall_tolerant=true;
        h->sim->pattern=a->sim_pattern<0?LR2021_SIM_COUNTER:a->sim_pattern;
        h->sim->tone_hz=a->sim_tone_hz?a->sim_tone_hz:154810000u;
        bus=lr2021_sim_bus(h->sim);
    } else {
        if(lrh_parse_board(a,&board)){ snprintf(err,n,"bad tcxo or rfsw argument"); return -1; }
        if(!(h->bus=lrh_spidev_open(&bus,a,err,n)))return -1;
    }
    if(!(h->dev=calloc(1,sizeof(*h->dev))))return -1;
    const lr2021_iq_rx rx={154785000,31250,13,0,LR2021_IQ_PATH_AUTO,0,0,0};
    int e=lr2021_iq_dev_open(h->dev,&bus,&board,&rx);
    if(e){ snprintf(err,n,"radio setup failed: %s",lr2021_iq_strerror(e)); return -1; }
    lrh_fd sv[2];
    if(lrh_socketpair(sv)){ snprintf(err,n,"socketpair: %s",strerror(errno)); return -1; }
    h->fd=sv[0]; h->server_fd=sv[1];
    static char ids[2][32];
    bool sim=h->backend==LR2021_HOST_SIM;
    snprintf(ids[sim],sizeof(ids[0]),"%.31s",sim?"sim":a->spidev);
    lr2021_bridge_radio radio={h->dev,&h->dev->hal,&h->dev->board,sim_configure,rx,
        (uint8_t)h->dev->profile.fw_major,(uint8_t)h->dev->profile.fw_minor,
        LR2021_BRIDGE_CAP_RX|LR2021_BRIDGE_CAP_TX|LR2021_BRIDGE_CAP_TIMED_TX|
        LR2021_BRIDGE_CAP_LIVE_GAIN|LR2021_BRIDGE_CAP_RATE_250K,
        sim?"lr2021-sdr simulator":"lr2021-sdr local spidev",ids[sim]};
    if(!(h->server=lrh_server_start(h->server_fd,&radio,LR2021_BRIDGE_TX_CAPACITY))) {
        snprintf(err,n,"cannot start server"); return -1;
    }
    return 0;
}
void lr2021_host_close(lr2021_host *h)
{
    if(!h)return;
    if(h->fd!=LRH_NO_FD && !h->link_down) {
        if(h->streaming)lr2021_host_rx_stop(h,NULL);
        if(h->backend==LR2021_HOST_SERIAL)request(h,LR2021_BRIDGE_BYE,NULL,0,NULL,0,NULL,300);
    }
    lrh_lock(&h->mu); h->closing=true; lrh_unlock(&h->mu);
    if(h->fd!=LRH_NO_FD && h->backend!=LR2021_HOST_SERIAL)lrh_shutdown(h->fd);
    if(h->server) {
        lrh_shutdown(h->server_fd);
        lrh_server_wait(h->server); lrh_server_free(h->server);
    }
    if(h->reader_running)lrh_thread_join(h->reader);
    if(h->fd!=LRH_NO_FD)lrh_close(h->fd);
    if(h->server_fd!=LRH_NO_FD)lrh_close(h->server_fd);
    if(h->bus)lrh_spidev_close(h->bus);
    free(h->dev); free(h->sim); free(h->ring);
    lrh_mutex_destroy(&h->mu); lrh_mutex_destroy(&h->req_mu); lrh_cond_destroy(&h->cv);
    free(h);
}
lr2021_host *lr2021_host_open(const lr2021_host_args *a, char *err, size_t n)
{
    char scratch[16];
    if(!err){ err=scratch; n=sizeof(scratch); }
    err[0]=0;
    lr2021_host *h=calloc(1,sizeof(*h));
    if(!h){ snprintf(err,n,"out of memory"); return NULL; }
    h->fd=h->server_fd=LRH_NO_FD;
    lrh_mutex_init(&h->mu); lrh_mutex_init(&h->req_mu); lrh_cond_init(&h->cv);
    lr2021_bridge_parser_init(&h->parser);
    h->ring=malloc(sizeof(block)*RING_BLOCKS);
    h->timeout_ms=a->timeout_ms?a->timeout_ms:2000; h->baud=a->baud?a->baud:LR2021_HOST_DEFAULT_BAUD;
    h->format=a->format;
    h->backend=a->backend?a->backend:a->sim_pattern>=0?LR2021_HOST_SIM:a->spidev[0]?LR2021_HOST_LOCAL:
        a->host[0]?LR2021_HOST_TCP:a->serial[0]?LR2021_HOST_SERIAL:0;
    int e=h->ring?0:-1;
    if(!e && lrh_net_init()){ snprintf(err,n,"network start-up failed"); e=-1; }
    if(!e) switch(h->backend) {
    case LR2021_HOST_SERIAL:
        h->fd=lrh_serial_open(a->serial,h->baud,err,n);
        if(h->fd==LRH_NO_FD)e=-1;
        else if(a->boot_ms){ lrh_sleep_us(a->boot_ms*1000u); lrh_serial_flush(h->fd); }
        break;
    case LR2021_HOST_TCP:
        h->fd=lrh_tcp_connect(a->host,a->port,h->timeout_ms,err,n);
        if(h->fd==LRH_NO_FD)e=-1;
        break;
    case LR2021_HOST_SIM: case LR2021_HOST_LOCAL:
        e=start_local(h,a,err,n);
        break;
    default:
        snprintf(err,n,"no device: set serial, host, spidev or sim"); e=-1;
    }
    if(!e) {
        h->reader_running=!lrh_thread_start(&h->reader,reader,h);
        e=h->reader_running?0:-1;
    }
    if(!e) {
        e=h->backend==LR2021_HOST_SERIAL?serial_enter(h):hello(h,h->timeout_ms);
        if(e)snprintf(err,n,"bridge handshake failed: %s",lr2021_host_strerror(e));
    }
    if(e){ if(!err[0])snprintf(err,n,"open failed"); lr2021_host_close(h); return NULL; }
    return h;
}
int lr2021_host_backend(const lr2021_host *h) { return h->backend; }
const lr2021_bridge_hello *lr2021_host_hello(const lr2021_host *h) { return &h->hello; }

int lr2021_host_set_rx(lr2021_host *h, const lr2021_bridge_rx *rx, lr2021_bridge_rx_ack *ack)
{
    uint8_t p[16], r[16]; size_t n=0;
    size_t len=lr2021_bridge_pack_rx(p,rx);
    int e=request(h,LR2021_BRIDGE_SET_RX,p,len,r,sizeof(r),&n,h->timeout_ms+3000);
    if(ack){ ack->status=e; ack->radio_errors=n>=6?lr2021_bridge_get16(r):0; ack->rate_hz=n>=6?lr2021_bridge_get32(r+2):0; }
    return e;
}
int lr2021_host_set_gain(lr2021_host *h, unsigned step, unsigned boost)
{
    if(step>13 || boost>7)return LR2021_HOST_EARG;
    uint8_t p[2]={(uint8_t)step,(uint8_t)boost};
    return request(h,LR2021_BRIDGE_SET_GAIN,p,2,NULL,0,NULL,h->timeout_ms+3000);
}
int lr2021_host_rx_start(lr2021_host *h, unsigned duration_ms)
{
    uint8_t p[7], r[8]; size_t n=0;
    lr2021_bridge_put16(p,1024); lr2021_bridge_put32(p+2,duration_ms); p[6]=(uint8_t)h->format;
    lrh_lock(&h->mu);
    /* Accept the new generation's data as soon as it arrives. */
    uint32_t next=h->generation+1;
    h->head=h->tail=h->offset=0; h->host_overflow=false; h->ended=false; h->expect_valid=false;
    h->generation=0xFFFFFFFFu; /* nothing matches until the ack */
    lrh_unlock(&h->mu);
    int e=request(h,LR2021_BRIDGE_RX_START,p,h->format?7:6,r,sizeof(r),&n,h->timeout_ms+3000);
    lrh_lock(&h->mu);
    if(!e && n>=8) {
        h->generation=lr2021_bridge_get32(r); h->rate_hz=lr2021_bridge_get32(r+4);
        h->streaming=true; h->ended=false;
    } else { h->generation=next-1; if(!e)e=LR2021_HOST_EPROTO; }
    lrh_unlock(&h->mu);
    return e;
}
long lr2021_host_rx_read(lr2021_host *h, int16_t *iq, size_t max, unsigned timeout_us,
                         lr2021_host_rx_meta *m)
{
    lr2021_host_rx_meta meta={0};
    long got=0;
    lrh_lock(&h->mu);
    if(h->head==h->tail && !h->ended && !h->link_down && timeout_us) {
        uint64_t t=lrh_now_us()+timeout_us;
        while(h->head==h->tail && !h->ended && !h->link_down)
            if(!lrh_cond_wait_until(&h->cv,&h->mu,t))break;
    }
    while(h->head!=h->tail && (size_t)got<max) {
        block *b=&h->ring[h->tail%RING_BLOCKS];
        uint64_t first=b->first_pair+h->offset;
        if(!got) {
            meta.first_pair=first; meta.generation=b->generation; meta.rate_hz=b->rate_hz;
            meta.time_ns=b->start_time*1000u+(b->rate_hz?first*1000000000u/b->rate_hz:0);
            meta.discontinuity=h->host_overflow || (h->expect_valid && first!=h->expect_pair);
            h->host_overflow=false;
        } else if(first!=meta.first_pair+(uint64_t)got)break;
        size_t take=b->pairs-h->offset;
        if(take>max-(size_t)got)take=max-(size_t)got;
        memcpy(iq+2*got,b->iq+2*h->offset,take*4);
        got+=(long)take; h->offset+=take;
        if(h->offset==b->pairs){ h->tail++; h->offset=0; }
    }
    if(got){ h->expect_pair=meta.first_pair+(uint64_t)got; h->expect_valid=true; }
    meta.ended=h->head==h->tail && h->ended;
    bool down=h->link_down;
    lrh_unlock(&h->mu);
    if(m)*m=meta;
    return got?got:down?LR2021_HOST_ELINK:0;
}
bool lr2021_host_rx_active(lr2021_host *h)
{ lrh_lock(&h->mu); bool s=h->streaming; lrh_unlock(&h->mu); return s; }
int lr2021_host_rx_stop(lr2021_host *h, lr2021_bridge_rx_end *end)
{
    int e=request(h,LR2021_BRIDGE_RX_STOP,NULL,0,NULL,0,NULL,h->timeout_ms);
    lrh_lock(&h->mu);
    uint64_t t=deadline(h->timeout_ms+2000);
    while(h->streaming && !h->ended && !h->link_down)
        if(!lrh_cond_wait_until(&h->cv,&h->mu,t))break;
    if(!e && !h->ended)e=h->link_down?LR2021_HOST_ELINK:LR2021_HOST_ETIMEOUT;
    if(end)*end=h->end;
    h->streaming=false;
    lrh_unlock(&h->mu);
    return e;
}
int lr2021_host_tx(lr2021_host *h, const int8_t *iq, size_t pairs, const lr2021_bridge_tx_begin *p,
                   lr2021_bridge_tx_end *end)
{
    if(!iq || !pairs || !p || 2*pairs>h->hello.tx_capacity)return LR2021_HOST_EARG;
    if(lr2021_host_rx_active(h))return LR2021_BRIDGE_EBUSY;
    lr2021_bridge_tx_begin b=*p;
    b.bytes=(uint32_t)(2*pairs); b.crc32=lr2021_bridge_crc32(0,(const uint8_t *)iq,b.bytes);
    uint8_t buf[LR2021_BRIDGE_TX_CHUNK+8];
    size_t len=lr2021_bridge_pack_tx_begin(buf,&b);
    int e=request(h,LR2021_BRIDGE_TX_BEGIN,buf,len,NULL,0,NULL,h->timeout_ms);
    for(uint32_t off=0;!e && off<b.bytes;off+=LR2021_BRIDGE_TX_CHUNK) {
        uint32_t m=b.bytes-off>LR2021_BRIDGE_TX_CHUNK?LR2021_BRIDGE_TX_CHUNK:b.bytes-off;
        uint8_t r[4]; size_t rn=0;
        lr2021_bridge_put32(buf,off); memcpy(buf+4,iq+off,m);
        e=request(h,LR2021_BRIDGE_TX_DATA,buf,4+m,r,4,&rn,h->timeout_ms);
        if(!e && (rn<4 || lr2021_bridge_get32(r)!=off+m))e=LR2021_HOST_EPROTO;
    }
    lrh_lock(&h->mu); h->tx_done=false; lrh_unlock(&h->mu);
    if(!e)e=request(h,LR2021_BRIDGE_TX_COMMIT,NULL,0,NULL,0,NULL,h->timeout_ms);
    if(e)return e;
    /* Burst (<= 0.9 s), a scheduled start (<= 10 s), reset and restore. */
    uint64_t wait_ms=h->timeout_ms+3000u+(uint64_t)pairs*b.repeats*1000u/b.rate_hz;
    if(b.start_time) {
        uint64_t now=0;
        if(!lr2021_host_time(h,&now) && b.start_time>now)wait_ms+=(b.start_time-now)/1000u;
    }
    lrh_lock(&h->mu);
    uint64_t t=deadline((unsigned)wait_ms);
    while(!h->tx_done && !h->link_down)
        if(!lrh_cond_wait_until(&h->cv,&h->mu,t))break;
    bool done=h->tx_done; lr2021_bridge_tx_end r=h->tx_end;
    lrh_unlock(&h->mu);
    if(!done)return h->link_down?LR2021_HOST_ELINK:LR2021_HOST_ETIMEOUT;
    if(end)*end=r;
    return r.error?r.error:r.stop_error?r.stop_error:r.recovery_error;
}
int lr2021_host_time(lr2021_host *h, uint64_t *us)
{
    uint8_t r[8]; size_t n=0;
    int e=request(h,LR2021_BRIDGE_GET_TIME,NULL,0,r,8,&n,h->timeout_ms);
    if(!e && n<8)e=LR2021_HOST_EPROTO;
    if(!e && us)*us=lr2021_bridge_get64(r);
    return e;
}

#ifndef _WIN32
static bool usb_ids(const char *tty, unsigned *vid, unsigned *pid)
{
#ifdef __linux__
    char path[512], real[PATH_MAX];
    snprintf(path,sizeof(path),"/sys/class/tty/%s/device",tty);
    if(!realpath(path,real))return false;
    /* Walk up from the interface to the USB device that has the IDs. */
    for(int depth=0;depth<4;depth++) {
        char f[PATH_MAX+16]; FILE *fp;
        snprintf(f,sizeof(f),"%s/idVendor",real);
        if((fp=fopen(f,"r"))) {
            bool ok=fscanf(fp,"%x",vid)==1; fclose(fp);
            snprintf(f,sizeof(f),"%s/idProduct",real);
            if(ok && (fp=fopen(f,"r"))){ ok=fscanf(fp,"%x",pid)==1; fclose(fp); return ok; }
            return false;
        }
        char *slash=strrchr(real,'/');
        if(!slash || slash==real)return false;
        *slash=0;
    }
#else
    (void)tty; (void)vid; (void)pid;
#endif
    return false;
}
#endif
size_t lr2021_host_list_serial(char paths[][256], size_t max)
{
    size_t n=0;
#ifdef _WIN32
    (void)paths; (void)max; /* not implemented: name the COM port */
#elif defined(__linux__)
    DIR *d=opendir("/sys/class/tty");
    struct dirent *e;
    while(d && (e=readdir(d)) && n<max) {
        unsigned vid=0,pid=0;
        if(strncmp(e->d_name,"ttyACM",6) && strncmp(e->d_name,"ttyUSB",6))continue;
        if(!usb_ids(e->d_name,&vid,&pid))continue;
        if((vid==0x1A86 && pid==0x55D3) || (vid==0x303A && (pid==0x1001 || pid==0x4001)))
            snprintf(paths[n++],256,"/dev/%.200s",e->d_name);
    }
    if(d)closedir(d);
#else
    const char *pats[]={"/dev/cu.wchusbserial*","/dev/cu.usbmodem*"};
    for(unsigned k=0;k<2;k++) {
        glob_t g;
        if(glob(pats[k],0,NULL,&g))continue;
        for(size_t i=0;i<g.gl_pathc && n<max;i++)snprintf(paths[n++],256,"%s",g.gl_pathv[i]);
        globfree(&g);
    }
    (void)usb_ids;
#endif
    return n;
}
