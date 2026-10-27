/* SPDX-License-Identifier: MIT */
/* rtl_tcp-compatible server over the LR2021 host client; see lr2021_tcp.h and
 * docs/rtl-tcp.md. Single threaded: one loop reads client commands, applies
 * them between reads of the receive stream, converts CS16 to u8 and sends. */
#define _DEFAULT_SOURCE
#include "lr2021_tcp.h"
#include "../src/host/host_internal.h"
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define BUF_PAIRS 4096u
#define READ_US 20000u

/* Gain values of the R820T tuner in tenths of a dB, the table rtl_tcp clients
 * show for tuner type 5. */
const int lr2021_tcp_r820t_gains[LR2021_TCP_GAINS]={
    0,9,14,27,37,77,87,125,144,157,166,197,207,229,254,
    280,297,328,338,364,372,386,402,421,434,439,445,480,496};

typedef struct {
    uint32_t frequency_hz, rate_hz;
    int ppm;
    unsigned path, level;               /* level 1..20, kept while in AGC */
    bool tuner_auto, rtl_agc;           /* either selects the chip AGC */
} settings;
typedef struct {
    lrh_fd fd;
    char peer[64];
    uint8_t cmd[5]; unsigned cmd_len;
    uint32_t ignored;                   /* commands already reported as unsupported */
    uint64_t expect; bool expect_valid;
    uint64_t started_us, pairs, lost, clipped, values;
    unsigned gaps, restarts;
    uint64_t window_lost, window_clipped, window_values; unsigned window_gaps, window_restarts;
    uint64_t drop_report_us, clip_report_us, end_report_us;
} client;
struct lr2021_tcp {
    lr2021_tcp_config cfg;
    lr2021_host *host;
    lrh_fd listener;
    atomic_bool stop;
    settings want, have;
    bool have_valid, streaming, have_250k;
    int16_t iq[2*BUF_PAIRS];
    uint8_t out[2*BUF_PAIRS];
};

static void say(lr2021_tcp *t, bool always, const char *fmt, ...)
{
    if(t->cfg.quiet && !always)return;
    FILE *f=t->cfg.log?t->cfg.log:stderr;
    va_list ap; va_start(ap,fmt);
    fputs("lr2021_tcp: ",f); vfprintf(f,fmt,ap); fputc('\n',f); fflush(f);
    va_end(ap);
}

uint32_t lr2021_tcp_snap_rate(uint32_t requested, bool have_250k)
{ return have_250k && requested>=187500u ? 250000u : 125000u; }
unsigned lr2021_tcp_gain_index(int tenth_db)
{
    unsigned best=0;
    for(unsigned i=1;i<LR2021_TCP_GAINS;i++)
        if(abs(lr2021_tcp_r820t_gains[i]-tenth_db)<abs(lr2021_tcp_r820t_gains[best]-tenth_db))best=i;
    return best;
}
unsigned lr2021_tcp_index_level(unsigned index)
{
    if(index>=LR2021_TCP_GAINS)index=LR2021_TCP_GAINS-1;
    return 1u+(index*(LR2021_TCP_LEVELS-1u)+(LR2021_TCP_GAINS-1u)/2u)/(LR2021_TCP_GAINS-1u);
}
void lr2021_tcp_level_gain(unsigned level, uint8_t *step, uint8_t *boost)
{
    if(level<1)level=1;
    if(level>LR2021_TCP_LEVELS)level=LR2021_TCP_LEVELS;
    *step=(uint8_t)(level<13?level:13); *boost=(uint8_t)(level>13?level-13:0);
}
/* A reference that runs ppm fast puts the synthesizer ppm high: ask for less. */
uint32_t lr2021_tcp_correct(uint32_t f, int ppm)
{
    double v=floor((double)f*1e6/(1e6+(double)ppm)+0.5);
    return v<0?0:v>4294967295.0?UINT32_MAX:(uint32_t)v;
}
size_t lr2021_tcp_convert(const int16_t *iq, uint8_t *out, size_t pairs, double scale)
{
    size_t clipped=0;
    for(size_t i=0;i<2*pairs;i++) {
        double v=floor((double)iq[i]*scale)+128.0;
        if(v<0){ v=0; clipped++; }
        else if(v>255){ v=255; clipped++; }
        out[i]=(uint8_t)v;
    }
    return clipped;
}

static void gain_of(const settings *s, uint8_t *step, uint8_t *boost)
{
    if(s->tuner_auto || s->rtl_agc){ *step=0; *boost=0; }
    else lr2021_tcp_level_gain(s->level,step,boost);
}
static const char *gain_text(const settings *s, char *buf, size_t n)
{
    uint8_t step, boost; gain_of(s,&step,&boost);
    if(!step)snprintf(buf,n,"chip AGC");
    else snprintf(buf,n,"level %u (step %u, boost %u)",s->level,step,boost);
    return buf;
}
static bool same_tune(const settings *a, const settings *b)
{ return a->frequency_hz==b->frequency_hz && a->rate_hz==b->rate_hz && a->ppm==b->ppm && a->path==b->path; }
static bool same(const settings *a, const settings *b)
{
    uint8_t s1,b1,s2,b2; gain_of(a,&s1,&b1); gain_of(b,&s2,&b2);
    return same_tune(a,b) && s1==s2 && b1==b2;
}
static int set_rx(lr2021_tcp *t, const settings *s)
{
    uint8_t step, boost; gain_of(s,&step,&boost);
    lr2021_bridge_rx rx={lr2021_tcp_correct(s->frequency_hz,s->ppm),s->rate_hz/4u,step,boost,(uint8_t)s->path,0,0,0};
    lr2021_bridge_rx_ack ack={0};
    int e=lr2021_host_set_rx(t->host,&rx,&ack);
    if(!e && ack.radio_errors)
        say(t,true,"radio error flags 0x%04x after tuning to %u Hz (IMG_CAL is expected below 104 MHz)",
            (unsigned)ack.radio_errors,(unsigned)rx.frequency_hz);
    return e;
}
/* Brings the device to t->want: a live gain change while streaming, else
 * stop, configure and start. The chip takes a new boost only between
 * sessions, so a boost change restarts the stream. A rejected frequency
 * reverts to the last good one, keeping the other new settings when the
 * device takes them. Returns a host error only when the device is unusable. */
static int apply(lr2021_tcp *t, client *c)
{
    char g[64];
    uint8_t step, boost, have_step, have_boost;
    gain_of(&t->want,&step,&boost); gain_of(&t->have,&have_step,&have_boost);
    if(t->streaming && t->have_valid && same_tune(&t->want,&t->have) && boost==have_boost) {
        if(same(&t->want,&t->have)){ t->have=t->want; return 0; }
        int e=lr2021_host_set_gain(t->host,step,boost);
        if(e==LR2021_HOST_ELINK)return e;
        if(e) {
            say(t,true,"gain %s rejected: %s",gain_text(&t->want,g,sizeof(g)),lr2021_host_strerror(e));
            t->want=t->have; return 0;
        }
        say(t,false,"gain %s",gain_text(&t->want,g,sizeof(g)));
        t->have=t->want;
        return 0;
    }
    if(t->streaming) {
        int e=lr2021_host_rx_stop(t->host,NULL);
        t->streaming=false;
        if(e==LR2021_HOST_ELINK)return e;
    }
    int e=set_rx(t,&t->want);
    if(e==LR2021_HOST_ELINK)return e;
    if(e) {
        say(t,true,"%u Hz at %u samples/s rejected by the device: %s; staying at %u Hz",
            (unsigned)t->want.frequency_hz,(unsigned)t->want.rate_hz,lr2021_host_strerror(e),
            (unsigned)t->have.frequency_hz);
        t->want.frequency_hz=t->have.frequency_hz;
        if(set_rx(t,&t->want)) {
            t->want=t->have;
            if((e=set_rx(t,&t->want)))return e;
        }
        t->have=t->want;
    } else {
        say(t,false,"tuned %u Hz%s, %u samples/s, %s",(unsigned)t->want.frequency_hz,
            t->want.ppm?" (corrected)":"",(unsigned)t->want.rate_hz,gain_text(&t->want,g,sizeof(g)));
        t->have=t->want; t->have_valid=true;
    }
    if((e=lr2021_host_rx_start(t->host,0)))return e;
    t->streaming=true;
    if(c)c->expect_valid=false;
    return 0;
}

static const char *command_name(uint8_t cmd)
{
    switch(cmd) {
    case LR2021_TCP_SET_IF_GAIN: return "IF gain";
    case LR2021_TCP_SET_TEST_MODE: return "test mode";
    case LR2021_TCP_SET_DIRECT: return "direct sampling";
    case LR2021_TCP_SET_OFFSET: return "offset tuning";
    case LR2021_TCP_SET_RTL_XTAL: return "RTL crystal";
    case LR2021_TCP_SET_TUNER_XTAL: return "tuner crystal";
    case LR2021_TCP_SET_BIAS_TEE: return "bias tee";
    default: return "unknown command";
    }
}
static void ignore(lr2021_tcp *t, client *c, uint8_t cmd, uint32_t v)
{
    uint32_t bit=1u<<(cmd<31?cmd:31);
    if(c->ignored&bit)return;
    c->ignored|=bit;
    say(t,false,"%s (0x%02x, value %u) not supported by the LR2021; ignored",command_name(cmd),cmd,(unsigned)v);
}
static void command(lr2021_tcp *t, client *c, uint8_t cmd, uint32_t v)
{
    settings *w=&t->want;
    switch(cmd) {
    case LR2021_TCP_SET_FREQ: w->frequency_hz=v; break;
    case LR2021_TCP_SET_RATE: {
        uint32_t r=lr2021_tcp_snap_rate(v,t->have_250k);
        if(r!=v)say(t,true,"sample rate %u requested; the LR2021 streams %u samples/s, set the client to %u",
                    (unsigned)v,(unsigned)r,(unsigned)r);
        w->rate_hz=r;
        break;
    }
    case LR2021_TCP_SET_GAIN_MODE: w->tuner_auto=!v; break;
    case LR2021_TCP_SET_GAIN: {
        unsigned i=lr2021_tcp_gain_index((int32_t)v);
        w->level=lr2021_tcp_index_level(i);
        say(t,false,"gain %.1f dB requested: R820T index %u, LR2021 level %u",(int32_t)v/10.0,i,w->level);
        break;
    }
    case LR2021_TCP_SET_PPM:
        if((int32_t)v<-1000 || (int32_t)v>1000)say(t,true,"correction %d ppm out of range; ignored",(int)(int32_t)v);
        else w->ppm=(int32_t)v;
        break;
    case LR2021_TCP_SET_AGC: w->rtl_agc=v!=0; break;
    case LR2021_TCP_SET_GAIN_INDEX: w->level=lr2021_tcp_index_level(v<LR2021_TCP_GAINS?v:LR2021_TCP_GAINS-1); break;
    /* Off is what the LR2021 does anyway; report only requests to turn on. */
    case LR2021_TCP_SET_TEST_MODE: case LR2021_TCP_SET_DIRECT: case LR2021_TCP_SET_OFFSET:
    case LR2021_TCP_SET_BIAS_TEE:
        if(v)ignore(t,c,cmd,v);
        break;
    default: ignore(t,c,cmd,v); break;
    }
}
/* Reads what the client has sent, waiting up to first_ms for the first bytes
 * and then until it pauses for next_ms; false when it has gone. */
static bool read_commands(lr2021_tcp *t, client *c, unsigned first_ms, unsigned next_ms)
{
    uint8_t buf[512];
    for(int rounds=0;rounds<64;rounds++) {
        int r=lrh_poll_read(c->fd,rounds?next_ms:first_ms);
        if(r<0)return false;
        if(!r)return true;
        long n=lrh_read(c->fd,buf,sizeof(buf));
        if(n==LRH_AGAIN)continue;
        if(n<=0)return false;
        for(long i=0;i<n;i++) {
            c->cmd[c->cmd_len++]=buf[i];
            if(c->cmd_len==5) {
                command(t,c,c->cmd[0],(uint32_t)c->cmd[1]<<24|(uint32_t)c->cmd[2]<<16|(uint32_t)c->cmd[3]<<8|c->cmd[4]);
                c->cmd_len=0;
            }
        }
    }
    return true;
}
static void reject_others(lr2021_tcp *t, client *c)
{
    char peer[64];
    while(lrh_poll_read(t->listener,0)==1) {
        lrh_fd fd=lrh_tcp_accept(t->listener,0,peer,sizeof(peer));
        if(fd==LRH_NO_FD)return;
        say(t,true,"%s refused: serving %s (one client at a time)",peer,c->peer);
        lrh_close(fd);
    }
}
static void report(lr2021_tcp *t, client *c, uint64_t now)
{
    if((c->window_gaps || c->window_restarts) && now-c->drop_report_us>=1000000u) {
        char more[80]="";
        if(c->window_restarts)snprintf(more,sizeof(more),", and %u stream restarts (length unknown)",c->window_restarts);
        say(t,true,"samples lost in the last %.1f s: %llu in %u gaps%s; %llu in total",
            (double)(now-c->drop_report_us)/1e6,(unsigned long long)c->window_lost,c->window_gaps,more,
            (unsigned long long)c->lost);
        c->window_gaps=c->window_restarts=0; c->window_lost=0; c->drop_report_us=now;
    }
    if(now-c->clip_report_us>=5000000u) {
        if(c->window_clipped)
            say(t,true,"%llu of %llu values clipped in the last %.0f s (%.3f%%): lower the gain or raise --shift",
                (unsigned long long)c->window_clipped,(unsigned long long)c->window_values,
                (double)(now-c->clip_report_us)/1e6,100.0*(double)c->window_clipped/(double)c->window_values);
        c->window_clipped=0; c->window_values=0; c->clip_report_us=now;
    }
}
static void drop_host(lr2021_tcp *t, int e)
{
    say(t,true,"device lost: %s; it is reopened for the next client",lr2021_host_strerror(e));
    lr2021_host_close(t->host);
    t->host=NULL; t->streaming=false;
}
static void session(lr2021_tcp *t, client *c)
{
    uint8_t hello[12]={'R','T','L','0',0,0,0,LR2021_TCP_TUNER_R820T,0,0,0,LR2021_TCP_GAINS};
    say(t,false,"client %s connected",c->peer);
    if(lrh_write_all(c->fd,hello,sizeof(hello)))return;
    uint64_t now=lrh_now_us();
    c->started_us=c->drop_report_us=c->clip_report_us=now;
    c->end_report_us=now-1000000u;
    /* Clients send their settings straight after the header; take them
     * before the first tune. */
    bool up=read_commands(t,c,200,30);
    int e=up?apply(t,c):0;
    while(up && !e && !atomic_load(&t->stop)) {
        if(!(up=read_commands(t,c,0,0)))break;
        if(!same(&t->want,&t->have) || !t->streaming) {
            if((e=apply(t,c)))break;
        }
        reject_others(t,c);
        lr2021_host_rx_meta m;
        long n=lr2021_host_rx_read(t->host,t->iq,BUF_PAIRS,READ_US,&m);
        now=lrh_now_us();
        if(n<0){ e=(int)n; break; }
        if(!n && m.ended) {
            lr2021_bridge_rx_end end={0};
            lr2021_host_rx_stop(t->host,&end);
            t->streaming=false;
            c->restarts++; c->window_restarts++;
            /* Every restart is counted in the report; describe one a second. */
            if(now-c->end_report_us>=1000000u) {
                c->end_report_us=now;
                say(t,true,"stream ended by the device (reason %u, capture error %d: %s, %u dropped); restarting",
                    (unsigned)end.reason,(int)end.capture_error,lr2021_host_strerror(end.capture_error),
                    (unsigned)end.dropped);
            }
            report(t,c,now);
            continue;
        }
        if(n>0) {
            if(m.discontinuity) {
                uint64_t lost=c->expect_valid && m.first_pair>c->expect ? m.first_pair-c->expect : 0;
                c->gaps++; c->window_gaps++; c->lost+=lost; c->window_lost+=lost;
            }
            c->expect=m.first_pair+(uint64_t)n; c->expect_valid=true;
            size_t clipped=lr2021_tcp_convert(t->iq,t->out,(size_t)n,t->cfg.scale);
            c->clipped+=clipped; c->window_clipped+=clipped; c->window_values+=2u*(uint64_t)n;
            c->pairs+=(uint64_t)n;
            if(lrh_write_all(c->fd,t->out,2u*(size_t)n)){ up=false; break; }
        }
        report(t,c,now);
    }
    if(e && e!=LR2021_HOST_ELINK)say(t,true,"device error: %s",lr2021_host_strerror(e));
    if(t->host && t->streaming) {
        int s=lr2021_host_rx_stop(t->host,NULL);
        if(!e)e=s==LR2021_HOST_ELINK?s:0;
        t->streaming=false;
    }
    if(e==LR2021_HOST_ELINK || (t->host && e))drop_host(t,e);
    double secs=(double)(lrh_now_us()-c->started_us)/1e6;
    say(t,c->gaps || c->restarts,"client %s gone after %.1f s: %llu samples sent, %llu lost in %u gaps, "
        "%u stream restarts, %llu values clipped",c->peer,secs,(unsigned long long)c->pairs,
        (unsigned long long)c->lost,c->gaps,c->restarts,(unsigned long long)c->clipped);
}

static int open_device(lr2021_tcp *t, char *err, size_t n)
{
    if(!(t->host=lr2021_host_open(&t->cfg.device,err,n)))return -1;
    const lr2021_bridge_hello *h=lr2021_host_hello(t->host);
    t->have_250k=(h->caps&LR2021_BRIDGE_CAP_RATE_250K)!=0;
    t->want.rate_hz=lr2021_tcp_snap_rate(t->want.rate_hz,t->have_250k);
    say(t,false,"device: %.32s (%.32s), ROM %u.%u",h->name,h->id,h->rom_major,h->rom_minor);
    int e=set_rx(t,&t->want);
    if(e) {
        snprintf(err,n,"%u Hz at %u samples/s: %s",(unsigned)t->want.frequency_hz,(unsigned)t->want.rate_hz,
                 lr2021_host_strerror(e));
        lr2021_host_close(t->host); t->host=NULL; return -1;
    }
    t->have=t->want; t->have_valid=true; t->streaming=false;
    return 0;
}
lr2021_tcp *lr2021_tcp_open(const lr2021_tcp_config *cfg, char *err, size_t n)
{
    lr2021_tcp *t=calloc(1,sizeof(*t));
    if(!t){ snprintf(err,n,"out of memory"); return NULL; }
    t->cfg=*cfg;
    if(t->cfg.scale<=0)t->cfg.scale=1.0/32.0;
    atomic_init(&t->stop,false);
    t->listener=LRH_NO_FD;
    settings *w=&t->want;
    w->frequency_hz=cfg->frequency_hz; w->rate_hz=cfg->rate_hz; w->ppm=cfg->ppm; w->path=cfg->path;
    w->tuner_auto=!cfg->level; w->level=cfg->level?cfg->level:13; w->rtl_agc=false;
    if(open_device(t,err,n)){ free(t); return NULL; }
    t->listener=lrh_tcp_listen(cfg->listen[0]?cfg->listen:"127.0.0.1",cfg->port,err,n);
    if(t->listener==LRH_NO_FD){ lr2021_tcp_close(t); return NULL; }
    say(t,false,"listening on %s:%u",cfg->listen[0]?cfg->listen:"127.0.0.1",lrh_tcp_port(t->listener));
    return t;
}
unsigned lr2021_tcp_port(const lr2021_tcp *t) { return lrh_tcp_port(t->listener); }
void lr2021_tcp_stop(lr2021_tcp *t) { atomic_store(&t->stop,true); }
int lr2021_tcp_serve(lr2021_tcp *t)
{
    while(!atomic_load(&t->stop)) {
        client *c=calloc(1,sizeof(*c));
        if(!c)return -1;
        c->fd=lrh_tcp_accept(t->listener,200,c->peer,sizeof(c->peer));
        if(c->fd!=LRH_NO_FD) {
            char err[200];
            if(!t->host && open_device(t,err,sizeof(err)))
                say(t,true,"client %s refused: device unavailable: %s",c->peer,err);
            else session(t,c);
            lrh_close(c->fd);
        }
        free(c);
    }
    return 0;
}
void lr2021_tcp_close(lr2021_tcp *t)
{
    if(!t)return;
    if(t->host)lr2021_host_close(t->host);
    if(t->listener!=LRH_NO_FD)lrh_close(t->listener);
    free(t);
}

void lr2021_tcp_config_init(lr2021_tcp_config *c)
{
    memset(c,0,sizeof(*c));
    lr2021_host_args_init(&c->device);
    snprintf(c->listen,sizeof(c->listen),"127.0.0.1");
    c->port=LR2021_TCP_PORT; c->frequency_hz=433920000u; c->rate_hz=250000u;
    c->level=13; c->path=LR2021_IQ_PATH_AUTO; c->scale=1.0/32.0;
}
void lr2021_tcp_usage(FILE *f, const char *p)
{
    fprintf(f,
"usage: %s -d DEVICE [options]\n"
"rtl_tcp-compatible server for an LR2021 (receive only)\n"
"  -d, --device ARGS   key=value pairs, comma separated, as for the SoapySDR\n"
"                      module: serial=COM5 or serial=/dev/ttyACM0 [,baud=,format=cs12],\n"
"                      host=192.168.4.1 [,port=], spidev=/dev/spidev0.0,..., sim=tone\n"
"  -a, --address ADDR  listen address (default 127.0.0.1; 0.0.0.0 for every interface)\n"
"  -p, --port N        listen port (default 1234)\n"
"  -f, --freq HZ       initial frequency, k/M/G suffixes allowed (default 433.92M)\n"
"  -s, --rate HZ       initial sample rate, snapped to 125000 or 250000 (default 250000)\n"
"  -g, --gain LEVEL    initial gain: auto (chip AGC) or 1..20; 1..13 is the gain step,\n"
"                      14..20 step 13 with boost 1..7 (default 13)\n"
"  -P, --ppm N         frequency correction until a client sets one (default 0)\n"
"      --antenna PATH  auto, lf or hf (default auto: HF from 1.5 GHz)\n"
"      --shift N       u8 = (sample >> N) + 128, N 0..15 (default 5)\n"
"      --scale F       u8 = floor(sample * F) + 128, instead of --shift\n"
"  -q, --quiet         log only problems: errors, lost samples, clipping\n"
"  -h, --help\n",p);
}
static int parse_hz(const char *s, uint32_t *out)
{
    char *end; double v=strtod(s,&end);
    if(end==s)return -1;
    if(*end=='k' || *end=='K'){ v*=1e3; end++; }
    else if(*end=='M'){ v*=1e6; end++; }
    else if(*end=='G'){ v*=1e9; end++; }
    if(*end || v<0 || v>4294967295.0)return -1;
    *out=(uint32_t)floor(v+0.5);
    return 0;
}
/* key=value,key=value; a part without '=' continues the previous value, so
 * rfsw=8:2,10:0 stays whole. */
static int parse_device(lr2021_host_args *a, const char *s, char *err, size_t n)
{
    char buf[1024], key[64]="", value[512]="";
    if(strlen(s)>=sizeof(buf)){ snprintf(err,n,"device argument too long"); return -1; }
    strcpy(buf,s);
    for(char *part=strtok(buf,",");;part=strtok(NULL,",")) {
        char *eq=part?strchr(part,'='):NULL;
        if(part && !eq) {
            if(!key[0] || strlen(value)+strlen(part)+2>sizeof(value)){ snprintf(err,n,"bad device argument near '%s'",part); return -1; }
            strcat(value,","); strcat(value,part); continue;
        }
        if(key[0]) {
            int r=lr2021_host_args_set(a,key,value);
            if(r){ snprintf(err,n,r>0?"unknown device key '%s'":"bad value for '%s'",key); return -1; }
        }
        if(!part)break;
        *eq=0;
        if(strlen(part)>=sizeof(key) || strlen(eq+1)>=sizeof(value)){ snprintf(err,n,"device argument too long"); return -1; }
        strcpy(key,part); strcpy(value,eq+1);
    }
    return 0;
}
int lr2021_tcp_parse(lr2021_tcp_config *c, int argc, char **argv, char *err, size_t n)
{
    bool device=false;
    for(int i=1;i<argc;i++) {
        const char *o=argv[i], *v=i+1<argc?argv[i+1]:NULL;
        bool takes=strcmp(o,"-q") && strcmp(o,"--quiet") && strcmp(o,"-h") && strcmp(o,"--help");
        if(takes && !v){ snprintf(err,n,"%s needs a value",o); return -1; }
        char *end=NULL;
        if(!strcmp(o,"-h") || !strcmp(o,"--help"))return 1;
        else if(!strcmp(o,"-q") || !strcmp(o,"--quiet")){ c->quiet=true; continue; }
        else if(!strcmp(o,"-d") || !strcmp(o,"--device")) {
            if(parse_device(&c->device,v,err,n))return -1;
            device=true;
        } else if(!strcmp(o,"-a") || !strcmp(o,"--address")) {
            if(strlen(v)>=sizeof(c->listen)){ snprintf(err,n,"address too long"); return -1; }
            strcpy(c->listen,v);
        } else if(!strcmp(o,"-p") || !strcmp(o,"--port")) {
            unsigned long p=strtoul(v,&end,10);
            if(*end || p>65535){ snprintf(err,n,"bad port '%s'",v); return -1; }
            c->port=(unsigned)p;
        } else if(!strcmp(o,"-f") || !strcmp(o,"--freq")) {
            if(parse_hz(v,&c->frequency_hz)){ snprintf(err,n,"bad frequency '%s'",v); return -1; }
        } else if(!strcmp(o,"-s") || !strcmp(o,"--rate")) {
            if(parse_hz(v,&c->rate_hz) || !c->rate_hz){ snprintf(err,n,"bad sample rate '%s'",v); return -1; }
        } else if(!strcmp(o,"-g") || !strcmp(o,"--gain")) {
            if(!strcmp(v,"auto") || !strcmp(v,"agc"))c->level=0;
            else {
                unsigned long g=strtoul(v,&end,10);
                if(*end || g<1 || g>LR2021_TCP_LEVELS){ snprintf(err,n,"gain is auto or 1..20, not '%s'",v); return -1; }
                c->level=(unsigned)g;
            }
        } else if(!strcmp(o,"-P") || !strcmp(o,"--ppm")) {
            long p=strtol(v,&end,10);
            if(*end || p<-1000 || p>1000){ snprintf(err,n,"bad ppm '%s'",v); return -1; }
            c->ppm=(int)p;
        } else if(!strcmp(o,"--antenna")) {
            if(!strcmp(v,"auto"))c->path=LR2021_IQ_PATH_AUTO;
            else if(!strcmp(v,"lf"))c->path=LR2021_IQ_PATH_LF;
            else if(!strcmp(v,"hf"))c->path=LR2021_IQ_PATH_HF;
            else { snprintf(err,n,"antenna is auto, lf or hf"); return -1; }
        } else if(!strcmp(o,"--shift")) {
            unsigned long s=strtoul(v,&end,10);
            if(*end || s>15){ snprintf(err,n,"shift is 0..15"); return -1; }
            c->scale=1.0/(double)(1u<<s);
        } else if(!strcmp(o,"--scale")) {
            double s=strtod(v,&end);
            if(*end || !(s>0) || s>256){ snprintf(err,n,"bad scale '%s'",v); return -1; }
            c->scale=s;
        } else { snprintf(err,n,"unknown option '%s'",o); return -1; }
        i++;
    }
    if(!device){ snprintf(err,n,"no device: give -d serial=..., host=... or sim=tone"); return -1; }
    return 0;
}
