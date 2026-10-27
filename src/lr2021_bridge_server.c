/* SPDX-License-Identifier: GPL-3.0-only */
#include "lr2021_bridge_server.h"
#include <string.h>

#define MASK (LR2021_BRIDGE_SLOTS-1u)
#define P32 lr2021_bridge_put32
#define G16 lr2021_bridge_get16
#define G32 lr2021_bridge_get32

static void send_frame(lr2021_bridge_server *s, uint8_t *buf, size_t cap, uint8_t type,
                       uint16_t seq, const uint8_t *payload, size_t n)
{
    size_t len=lr2021_bridge_encode(buf,cap,type,seq,payload,n);
    if(!len)return;
    if(s->io.lock)s->io.lock(s->io.context);
    if(s->io.send(s->io.context,buf,len))s->send_errors++;
    if(s->io.unlock)s->io.unlock(s->io.context);
}
static void reply(lr2021_bridge_server *s, uint8_t *buf, size_t cap, uint8_t type, uint16_t seq,
                  int32_t status, const uint8_t *extra, size_t n)
{
    uint8_t p[96];
    P32(p,(uint32_t)status);
    if(n)memcpy(p+4,extra,n);
    send_frame(s,buf,cap,(uint8_t)(type|LR2021_BRIDGE_RESPONSE),seq,p,4+n);
}
#define READER_REPLY(t,q,st,x,n) reply(s,s->reader_frame,sizeof(s->reader_frame),t,q,st,x,n)
#define WORKER_REPLY(t,q,st,x,n) reply(s,s->worker_frame,sizeof(s->worker_frame),t,q,st,x,n)

int lr2021_bridge_server_init(lr2021_bridge_server *s, const lr2021_bridge_io *io,
                              const lr2021_bridge_radio *radio, uint8_t *tx_buf, size_t tx_capacity)
{
    if(!s || !io || !io->send || !io->wake || !radio || !radio->hal || !radio->configure)return -1;
    memset(s,0,sizeof(*s));
    s->io=*io; s->radio=*radio; s->tx_buf=tx_buf; s->tx_capacity=tx_buf?tx_capacity:0;
    lr2021_bridge_parser_init(&s->parser);
    atomic_init(&s->busy,false); atomic_init(&s->streaming,false);
    atomic_init(&s->stop,false); atomic_init(&s->exit,false);
    atomic_init(&s->gain_request,0); atomic_init(&s->head,0); atomic_init(&s->tail,0);
    atomic_init(&s->live_boost,radio->rx.boost);
    atomic_init(&s->dropped,0); atomic_init(&s->tx_state,0);
    return 0;
}

/* Output ring: worker produces, sender consumes. */
/* RX data keeps one slot free so the RX_END that follows always fits. */
static lr2021_bridge_slot *slot_begin(lr2021_bridge_server *s, unsigned reserve)
{
    unsigned h=atomic_load_explicit(&s->head,memory_order_relaxed);
    unsigned t=atomic_load_explicit(&s->tail,memory_order_acquire);
    return h-t+reserve>=LR2021_BRIDGE_SLOTS ? NULL : &s->slots[h&MASK];
}
static void slot_commit(lr2021_bridge_server *s)
{
    unsigned h=atomic_load_explicit(&s->head,memory_order_relaxed);
    atomic_store_explicit(&s->head,h+1,memory_order_release);
}
static void slot_wait_put(lr2021_bridge_server *s, uint8_t type, uint16_t seq, const uint8_t *p, size_t n)
{
    lr2021_bridge_slot *b;
    const lr2021_iq_hal *h=s->radio.hal;
    for(unsigned i=0;!(b=slot_begin(s,0));i++) {
        if(i>=2000)return; /* sender stalled for 2 s: give up on this message */
        h->sleep_us(h->context,1000);
    }
    b->type=type; b->sequence=seq; b->length=(uint16_t)n; memcpy(b->payload,p,n);
    slot_commit(s);
}
bool lr2021_bridge_server_pump(lr2021_bridge_server *s)
{
    unsigned t=atomic_load_explicit(&s->tail,memory_order_relaxed);
    unsigned h=atomic_load_explicit(&s->head,memory_order_acquire);
    if(h==t)return false;
    lr2021_bridge_slot *b=&s->slots[t&MASK];
    send_frame(s,s->sender_frame,sizeof(s->sender_frame),b->type,b->sequence,b->payload,b->length);
    atomic_store_explicit(&s->tail,t+1,memory_order_release);
    return true;
}

/* Pairs accumulate in the open slot up to 480 per frame, then are packed in
 * the stream's format. A full queue drops whole chunks and counts them. */
static void flush(lr2021_bridge_server *s)
{
    lr2021_bridge_slot *b=s->cur;
    if(!b)return;
    s->cur=NULL;
    if(!s->cur_pairs)return;
    lr2021_bridge_rx_data d={s->generation,s->rate_hz,s->cur_first,s->start_time,
        atomic_load(&s->dropped),(uint16_t)s->cur_pairs,s->format,0};
    size_t m=lr2021_bridge_pack_samples(b->payload+LR2021_BRIDGE_RX_DATA_HEADER,s->format,
        s->staging,s->cur_pairs,&d.shift);
    lr2021_bridge_pack_rx_data(b->payload,&d);
    b->type=LR2021_BRIDGE_RX_DATA; b->sequence=s->seq_data++;
    b->length=(uint16_t)(LR2021_BRIDGE_RX_DATA_HEADER+m);
    slot_commit(s);
}
static bool sink(void *ctx, const uint8_t *data, size_t n, uint64_t first)
{
    lr2021_bridge_server *s=ctx;
    if(atomic_load(&s->stop))return false;
    size_t pairs=n/4, i=0;
    while(i<pairs) {
        if(s->cur && s->cur_first+s->cur_pairs!=first+i)flush(s);
        if(!s->cur) {
            if(!(s->cur=slot_begin(s,1))) {
                atomic_fetch_add(&s->dropped,(unsigned)(pairs-i)); return true;
            }
            s->cur_first=first+i; s->cur_pairs=0;
        }
        size_t take=LR2021_BRIDGE_RX_MAX_PAIRS-s->cur_pairs;
        if(take>pairs-i)take=pairs-i;
        memcpy(s->staging+4*s->cur_pairs,data+4*i,4*take);
        s->cur_pairs+=take; i+=take;
        if(s->cur_pairs==LR2021_BRIDGE_RX_MAX_PAIRS)flush(s);
    }
    return true;
}
static bool gain(void *ctx, uint8_t *step, uint8_t *boost)
{
    lr2021_bridge_server *s=ctx;
    unsigned g=atomic_exchange(&s->gain_request,0);
    if(!g)return false;
    *step=(uint8_t)g; *boost=(uint8_t)(g>>8);
    s->radio.rx.gain_step=*step; s->radio.rx.boost=*boost;
    return true;
}
typedef struct { lr2021_bridge_server *s; lr2021_iq_session *session; } stream_ctx;
static bool session_sink(void *ctx, const uint8_t *data, size_t n, uint64_t first)
{
    stream_ctx *c=ctx;
    c->s->start_time=c->session->start_us;
    return sink(c->s,data,n,first);
}
static void rx_start(lr2021_bridge_server *s, uint16_t seq, const uint8_t *p, size_t n)
{
    if(n!=6 && n!=7){ WORKER_REPLY(LR2021_BRIDGE_RX_START,seq,LR2021_BRIDGE_ELENGTH,NULL,0); return; }
    unsigned cap=G16(p); uint32_t ms=G32(p+2); uint8_t format=n==7?p[6]:LR2021_BRIDGE_CS16;
    if((cap!=256 && cap!=1024) || ms>60000 || format>LR2021_BRIDGE_CS12) {
        WORKER_REPLY(LR2021_BRIDGE_RX_START,seq,LR2021_IQ_ARGUMENT,NULL,0); return;
    }
    s->generation++;
    s->rate_hz=lr2021_iq_rate(s->radio.rx.bandwidth_hz);
    s->start_time=0; s->format=format; s->cur=NULL; s->cur_pairs=0;
    atomic_store(&s->dropped,0); atomic_store(&s->stop,false); atomic_store(&s->gain_request,0);
    atomic_store(&s->live_boost,s->radio.rx.boost);
    atomic_store(&s->streaming,true);
    uint8_t ack[8]; P32(ack,s->generation); P32(ack+4,s->rate_hz);
    WORKER_REPLY(LR2021_BRIDGE_RX_START,seq,0,ack,8);
    lr2021_iq_session session={ms,cap,gain,s,0};
    stream_ctx c={s,&session};
    lr2021_iq_result r;
    int e=lr2021_iq_stream_session(s->radio.hal,&session,session_sink,&c,&r);
    flush(s);
    atomic_store(&s->streaming,false);
    lr2021_bridge_rx_end end={s->generation,r.capture_error,r.recovery_error,
        (uint8_t)(e?LR2021_BRIDGE_END_ERROR:LR2021_BRIDGE_END_REQUEST),r.fifo_flags,
        (uint16_t)r.max_fifo_level,atomic_load(&s->dropped),r.gain_changes,r.validated_bytes/4,
        r.start_us,r.elapsed_us,r.max_service_gap_us};
    if(e && !end.capture_error && !end.recovery_error)end.capture_error=e;
    uint8_t buf[64];
    size_t len=lr2021_bridge_pack_rx_end(buf,&end);
    slot_wait_put(s,LR2021_BRIDGE_RX_END,s->seq_rx_end++,buf,len);
}
static void tx_commit(lr2021_bridge_server *s, uint16_t seq)
{
    int st=0;
    if(atomic_load(&s->tx_state)!=1 || s->tx_next!=s->tx.bytes)st=LR2021_BRIDGE_ESTATE;
    else if(lr2021_bridge_crc32(0,s->tx_buf,s->tx.bytes)!=s->tx.crc32)st=LR2021_BRIDGE_ECRC;
    lr2021_iq_tx t={(const int8_t *)s->tx_buf,s->tx.bytes/2,s->tx.repeats,s->tx.rate_hz,
        s->tx.frequency_hz,s->tx.power_half_db,s->tx.start_time,s->tx.pa,
        (uint8_t)(s->tx.flags&(LR2021_IQ_TX_UNCHECKED|LR2021_IQ_TX_SIMO))};
    if(!st && lr2021_iq_tx_check(&t))st=LR2021_BRIDGE_ERANGE;
    if(st) {
        atomic_store(&s->tx_state,0);
        WORKER_REPLY(LR2021_BRIDGE_TX_COMMIT,seq,st,NULL,0); return;
    }
    atomic_store(&s->tx_state,2);
    WORKER_REPLY(LR2021_BRIDGE_TX_COMMIT,seq,0,NULL,0);
    lr2021_iq_tx_result r;
    lr2021_iq_transmit(s->radio.hal,s->radio.board,&t,&r);
    /* The upload stays valid so the host may send it again with TX_COMMIT. */
    atomic_store(&s->tx_state,1);
    lr2021_bridge_tx_end end={r.error,r.stop_error,r.recovery_error,r.tx_flags,
        (uint16_t)r.final_level,(uint16_t)r.min_level,r.irq,(uint32_t)(r.queued_bytes/2),
        r.started_us,r.elapsed_us,r.max_service_gap_us,r.untested};
    uint8_t buf[64];
    size_t len=lr2021_bridge_pack_tx_end(buf,&end);
    slot_wait_put(s,LR2021_BRIDGE_TX_END,s->seq_tx_end++,buf,len);
}
static void configure(lr2021_bridge_server *s, uint8_t type, uint16_t seq, const lr2021_iq_rx *rx)
{
    lr2021_bridge_rx_ack ack={0,0,0};
    ack.status=lr2021_iq_rx_check(rx);
    if(!ack.status)ack.status=s->radio.configure(s->radio.context,rx,&ack.radio_errors);
    if(!ack.status)s->radio.rx=*rx;
    ack.rate_hz=lr2021_iq_rate(s->radio.rx.bandwidth_hz);
    uint8_t p[16];
    lr2021_bridge_pack_rx_ack(p,&ack);
    send_frame(s,s->worker_frame,sizeof(s->worker_frame),(uint8_t)(type|LR2021_BRIDGE_RESPONSE),seq,p,10);
}
void lr2021_bridge_server_work(lr2021_bridge_server *s)
{
    if(!atomic_load_explicit(&s->busy,memory_order_acquire))return;
    const uint8_t *p=s->req; size_t n=s->req_length; uint16_t seq=s->req_seq;
    switch(s->req_type) {
    case LR2021_BRIDGE_SET_RX: {
        lr2021_bridge_rx m;
        if(lr2021_bridge_unpack_rx(p,n,&m)){ WORKER_REPLY(s->req_type,seq,LR2021_BRIDGE_ELENGTH,NULL,0); break; }
        lr2021_iq_rx rx={m.frequency_hz,m.bandwidth_hz,m.gain_step,m.boost,m.path};
        configure(s,s->req_type,seq,&rx);
        break;
    }
    case LR2021_BRIDGE_SET_GAIN: {
        lr2021_iq_rx rx=s->radio.rx;
        rx.gain_step=p[0]; rx.boost=p[1];
        configure(s,s->req_type,seq,&rx);
        break;
    }
    case LR2021_BRIDGE_RX_START: rx_start(s,seq,p,n); break;
    case LR2021_BRIDGE_TX_COMMIT: tx_commit(s,seq); break;
    default: WORKER_REPLY(s->req_type,seq,LR2021_BRIDGE_EUNKNOWN,NULL,0); break;
    }
    atomic_store_explicit(&s->busy,false,memory_order_release);
}
static void post(lr2021_bridge_server *s, const lr2021_bridge_frame *f)
{
    bool expected=false;
    if(!atomic_compare_exchange_strong(&s->busy,&expected,true)) {
        READER_REPLY(f->type,f->sequence,LR2021_BRIDGE_EBUSY,NULL,0); return;
    }
    s->req_type=f->type; s->req_seq=f->sequence; s->req_length=f->length;
    memcpy(s->req,f->payload,f->length);
    atomic_thread_fence(memory_order_release);
    s->io.wake(s->io.context);
}
static void request(void *ctx, const lr2021_bridge_frame *f)
{
    lr2021_bridge_server *s=ctx;
    const uint8_t *p=f->payload; size_t n=f->length;
    uint8_t x[8];
    switch(f->type) {
    case LR2021_BRIDGE_HELLO: {
        lr2021_bridge_hello h={0,LR2021_BRIDGE_VERSION,s->radio.caps,s->radio.rom_major,
            s->radio.rom_minor,LR2021_BRIDGE_MAX_PAYLOAD,1000000,(uint32_t)s->tx_capacity,{0},{0}};
        h.caps|=LR2021_BRIDGE_CAP_CS12|LR2021_BRIDGE_CAP_TX_UNCHECKED;
        if(!s->tx_buf)h.caps&=(uint16_t)~(LR2021_BRIDGE_CAP_TX|LR2021_BRIDGE_CAP_TIMED_TX|LR2021_BRIDGE_CAP_TX_UNCHECKED);
        if(s->radio.name)strncpy(h.name,s->radio.name,31);
        if(s->radio.id)strncpy(h.id,s->radio.id,31);
        uint8_t buf[96]; size_t len=lr2021_bridge_pack_hello(buf,&h);
        uint8_t frame[LR2021_BRIDGE_HEADER+96+4];
        send_frame(s,frame,sizeof(frame),LR2021_BRIDGE_HELLO|LR2021_BRIDGE_RESPONSE,f->sequence,buf,len);
        break;
    }
    case LR2021_BRIDGE_GET_TIME: {
        const lr2021_iq_hal *h=s->radio.hal;
        lr2021_bridge_put64(x,h->now_us(h->context));
        READER_REPLY(f->type,f->sequence,0,x,8);
        break;
    }
    case LR2021_BRIDGE_RX_STOP:
        if(atomic_load(&s->streaming))atomic_store(&s->stop,true);
        READER_REPLY(f->type,f->sequence,0,NULL,0);
        break;
    case LR2021_BRIDGE_SET_GAIN:
        if(n!=2){ READER_REPLY(f->type,f->sequence,LR2021_BRIDGE_ELENGTH,NULL,0); break; }
        if(p[0]>13 || p[1]>7){ READER_REPLY(f->type,f->sequence,LR2021_IQ_ARGUMENT,NULL,0); break; }
        if(atomic_load(&s->streaming)) {
            /* ROM 1.24 refuses SetRxPath during RX, so a boost change cannot be
             * applied live: the host stops, sets it and restarts. */
            if(p[1]!=atomic_load(&s->live_boost)){ READER_REPLY(f->type,f->sequence,LR2021_BRIDGE_EBUSY,NULL,0); break; }
            atomic_store(&s->gain_request,0x10000u|(unsigned)p[1]<<8|p[0]);
            lr2021_bridge_rx_ack a={0,0,s->rate_hz}; uint8_t q[16];
            lr2021_bridge_pack_rx_ack(q,&a);
            send_frame(s,s->reader_frame,sizeof(s->reader_frame),f->type|LR2021_BRIDGE_RESPONSE,f->sequence,q,10);
        } else post(s,f);
        break;
    case LR2021_BRIDGE_TX_BEGIN: {
        lr2021_bridge_tx_begin b;
        int st=0;
        if(!s->tx_buf)st=LR2021_BRIDGE_EUNSUPPORTED;
        else if(lr2021_bridge_unpack_tx_begin(p,n,&b))st=LR2021_BRIDGE_ELENGTH;
        else if(atomic_load(&s->tx_state)==2)st=LR2021_BRIDGE_EBUSY;
        else if(b.bytes<2 || b.bytes%2 || b.bytes>s->tx_capacity || !b.repeats || b.pa>2)
            st=LR2021_BRIDGE_ERANGE;
        if(!st){ s->tx=b; s->tx_next=0; atomic_store(&s->tx_state,1); }
        else if(st!=LR2021_BRIDGE_EBUSY)atomic_store(&s->tx_state,0);
        P32(x,0);
        READER_REPLY(f->type,f->sequence,st,x,4);
        break;
    }
    case LR2021_BRIDGE_TX_DATA: {
        int st=0;
        uint32_t off=n>=4?G32(p):0; size_t m=n>=4?n-4:0;
        if(atomic_load(&s->tx_state)!=1)st=LR2021_BRIDGE_ESTATE;
        else if(n<6 || m%2 || m>LR2021_BRIDGE_TX_CHUNK)st=LR2021_BRIDGE_ELENGTH;
        else if(off!=s->tx_next || m>s->tx.bytes-s->tx_next)st=LR2021_BRIDGE_ERANGE;
        if(!st){ memcpy(s->tx_buf+off,p+4,m); s->tx_next+=(uint32_t)m; }
        P32(x,s->tx_next);
        READER_REPLY(f->type,f->sequence,st,x,4);
        break;
    }
    case LR2021_BRIDGE_BYE:
        if(atomic_load(&s->streaming))atomic_store(&s->stop,true);
        READER_REPLY(f->type,f->sequence,0,NULL,0);
        atomic_store(&s->exit,true);
        break;
    case LR2021_BRIDGE_SET_RX: case LR2021_BRIDGE_RX_START: case LR2021_BRIDGE_TX_COMMIT:
        post(s,f);
        break;
    default:
        if(!(f->type&LR2021_BRIDGE_RESPONSE))READER_REPLY(f->type,f->sequence,LR2021_BRIDGE_EUNKNOWN,NULL,0);
        break;
    }
}
void lr2021_bridge_server_input(lr2021_bridge_server *s, const uint8_t *data, size_t n)
{ lr2021_bridge_parse(&s->parser,data,n,request,s); }
bool lr2021_bridge_server_exit_requested(lr2021_bridge_server *s) { return atomic_load(&s->exit); }
void lr2021_bridge_server_stop(lr2021_bridge_server *s) { if(atomic_load(&s->streaming))atomic_store(&s->stop,true); }
