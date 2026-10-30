/* SPDX-License-Identifier: MIT */
#include "lr2021_iq_dev.h"
#include <string.h>

unsigned lr2021_iq_rate(uint32_t bw)
{ return bw==31250 || bw==62500 || bw==125000 || bw==250000 ? 4*bw : 0; }
uint32_t lr2021_iq_bandwidth(unsigned rate)
{ return lr2021_iq_rate(rate/4)==rate ? rate/4 : 0; }
unsigned lr2021_iq_input_rate(uint8_t code)
{
    static const unsigned num[4]={1,13,13,13}, den[4]={1,14,15,18};
    uint64_t d=(uint64_t)den[code>>6]*(((code>>3)&7u)+1u)<<(code&7u);
    return (unsigned)((8000000ull*num[code>>6]+d/2)/d);
}
unsigned lr2021_iq_rx_rate(const lr2021_iq_rx *c)
{
    if(c->route==LR2021_IQ_ROUTE_SOURCE1)return lr2021_iq_input_rate(c->input_code);
    return c->route && c->route!=LR2021_IQ_ROUTE_RAW0B ? c->rate_hz : lr2021_iq_rate(c->bandwidth_hz);
}
unsigned lr2021_iq_rx_pair_bytes(const lr2021_iq_rx *c) { return c->route ? 2u : 4u; }
unsigned lr2021_iq_path(uint32_t hz, unsigned path)
{ return path==LR2021_IQ_PATH_AUTO ? (hz>=LR2021_IQ_HF_HZ ? 1u : 0u) : path; }
int lr2021_iq_rx_check(const lr2021_iq_rx *c)
{
    return c && c->frequency_hz>=LR2021_IQ_MIN_HZ && c->frequency_hz<=LR2021_IQ_MAX_HZ &&
        c->route<=LR2021_IQ_ROUTE_RAW0B &&
        (c->route==LR2021_IQ_ROUTE_SOURCE1 ||
         (c->route && c->route!=LR2021_IQ_ROUTE_RAW0B ? c->rate_hz>=1000 && c->rate_hz<=4000000u
                                                      : lr2021_iq_rate(c->bandwidth_hz)!=0)) &&
        c->gain_step<=13 && c->boost<=7 && c->path<=LR2021_IQ_PATH_AUTO ? 0 : LR2021_IQ_ARGUMENT;
}
const char *lr2021_iq_strerror(int e)
{
    switch(e) {
    case LR2021_IQ_OK: return "ok";
    case LR2021_IQ_ARGUMENT: return "invalid argument";
    case LR2021_IQ_STATE: return "radio state or ROM version refused";
    case LR2021_IQ_TIMEOUT: return "timeout";
    case LR2021_IQ_DATA: return "radio data, status or FIFO error";
    case LR2021_IQ_IO: return "bus I/O error";
    default: return "error";
    }
}

static int put(const lr2021_iq_hal *h, uint16_t op, const uint8_t *a, size_t n)
{ return h->write(h->context,op,a,n); }
int lr2021_iq_setup(const lr2021_iq_hal *h, const lr2021_iq_board *b,
                    unsigned *major, unsigned *minor)
{
    uint8_t v[2]={0};
    int e=h->read(h->context,0x0101,v,2);
    if(major)*major=v[0];
    if(minor)*minor=v[1];
    if(e)return e;
    if(v[0]!=1 || v[1]!=24)return LR2021_IQ_STATE;
    e=h->standby(h->context);
    if(!e)e=put(h,0x0111,NULL,0);
    if(!e && b && b->tcxo) {
        uint32_t t=b->tcxo_ticks;
        const uint8_t a[]={b->tcxo_voltage,(uint8_t)(t>>24),(uint8_t)(t>>16),(uint8_t)(t>>8),(uint8_t)t};
        e=b->tcxo_voltage>7 ? LR2021_IQ_ARGUMENT : put(h,0x0120,a,sizeof(a));
    }
    uint8_t a=0;
    if(!e)e=put(h,0x0121,&a,1); /* LDO */
    a=1;
    if(!e)e=put(h,0x0206,&a,1); /* fallback standby RC */
    a=0x6F;
    if(!e)e=put(h,0x0122,&a,1);
    return e;
}
int lr2021_iq_board_rfsw(const lr2021_iq_hal *h, const lr2021_iq_board *b)
{
    int e=0;
    if(!b)return 0;
    if(b->rfsw_count>LR2021_IQ_RFSW_MAX)return LR2021_IQ_ARGUMENT;
    for(unsigned i=0;!e && i<b->rfsw_count;i++) {
        const lr2021_iq_rfsw *r=&b->rfsw[i];
        uint8_t a[]={r->dio,(uint8_t)(0x20|(r->pull&3))};
        if(r->dio<5 || r->dio>11 || r->modes>31)return LR2021_IQ_ARGUMENT;
        e=put(h,0x0112,a,2);
        a[1]=r->modes;
        if(!e)e=put(h,0x0113,a,2);
    }
    return e;
}
int lr2021_iq_configure(const lr2021_iq_hal *h, const lr2021_iq_rx *c, uint16_t *errors)
{
    if(errors)*errors=0;
    int e=lr2021_iq_rx_check(c);
    if(e)return e;
    unsigned path=lr2021_iq_path(c->frequency_hz,c->path), hz=c->frequency_hz;
    unsigned cell=(hz/4000000u)|(path?0x8000u:0);
    const uint8_t type=0, cal[6]={(uint8_t)(cell>>8),(uint8_t)cell,0,0,0,0};
    const uint8_t freq[4]={(uint8_t)(hz>>24),(uint8_t)(hz>>16),(uint8_t)(hz>>8),(uint8_t)hz};
    const uint8_t rxpath[2]={(uint8_t)path,c->boost}, step=c->gain_step;
    /* SF7, BW code 2..5 (31.25..250 kHz), CR 4/5; source-2 rate is 4 x BW. */
    const uint8_t bwcode=c->bandwidth_hz==31250?2:c->bandwidth_hz==62500?3:c->bandwidth_hz==125000?4:5;
    const uint8_t mod[2]={(uint8_t)(0x70|bwcode),0x10};
    /* Raw routes: FSK at 10 kbit/s, no shaping, RX filter code input_code,
     * 5 kHz deviation; the 0x0290 route sets the output rate. */
    const uint8_t fsk_type=2, fsk[9]={0,0,0x27,0x10,0,c->input_code,0,0x13,0x88};
    const uint8_t pkt[4]={0,8,255,2}, sync=0x12;
    e=h->standby(h->context);
    if(!e)e=put(h,0x0111,NULL,0);
    const bool fsk_route=c->route && c->route!=LR2021_IQ_ROUTE_RAW0B;
    if(!e)e=put(h,0x0207,fsk_route?&fsk_type:&type,1);
    if(!e)e=put(h,0x0123,cal,6);
    if(!e)e=put(h,0x0200,freq,4);
    if(!e)e=put(h,0x0201,rxpath,2);
    if(!e)e=put(h,0x021A,&step,1);
    if(!e && fsk_route)e=put(h,0x0240,fsk,9);
    if(!e && !fsk_route)e=put(h,0x0220,mod,2);
    if(!e && !fsk_route)e=put(h,0x0221,pkt,4);
    if(!e && !fsk_route)e=put(h,0x0223,&sync,1);
    uint8_t err[2]={0};
    if(!e)e=h->read(h->context,0x0110,err,2);
    unsigned v=((unsigned)err[0]<<8)|err[1];
    if(errors)*errors=(uint16_t)v;
    if(!e && (v&~LR2021_IQ_ERR_IMG_CAL))e=LR2021_IQ_DATA;
    return e;
}

/* HAL over the bus. Framing and status rules follow the ESP32-P4 adapter. */
static int ready(lr2021_iq_dev *d)
{
    uint64_t end=d->bus.now_us(d->bus.context)+200000;
    for(;;) {
        int b=d->bus.busy(d->bus.context);
        if(b<0)return LR2021_IQ_IO;
        if(!b)return 0;
        if(d->bus.now_us(d->bus.context)>=end)return LR2021_IQ_TIMEOUT;
    }
}
static int frame(lr2021_iq_dev *d, uint8_t *b, size_t n)
{
    int e=ready(d);
    if(e)return e;
    return d->bus.transfer(d->bus.context,b,n) ? LR2021_IQ_IO : 0;
}
static int status_ok(const uint8_t *b)
{
    unsigned s=((unsigned)b[0]<<8)|b[1];
    return s && !(s&0xF008) && ((s>>9)&7)>=2 && ((s>>9)&7)<=3 &&
        ((s>>4)&7)<=2 && (s&7)<=6 ? 0 : LR2021_IQ_DATA;
}
static int write_cmd(void *ctx, uint16_t op, const uint8_t *a, size_t n)
{
    lr2021_iq_dev *d=ctx;
    if(n>256)return LR2021_IQ_ARGUMENT;
    uint8_t b[258]={(uint8_t)(op>>8),(uint8_t)op};
    if(n)memcpy(b+2,a,n);
    int e=frame(d,b,n+2);
    if(e)return e;
    uint8_t s[6]={1,0}; /* GetStatus reports the previous command's status */
    e=frame(d,s,sizeof(s));
    return e?e:status_ok(s);
}
static int read_args(lr2021_iq_dev *d, uint16_t op, const uint8_t *a, size_t n, uint8_t *out, size_t m)
{
    if(n>256 || m>256)return LR2021_IQ_ARGUMENT;
    uint8_t b[258]={(uint8_t)(op>>8),(uint8_t)op};
    if(n)memcpy(b+2,a,n);
    int e=frame(d,b,n+2);
    if(e)return e;
    memset(b,0,m+2);
    e=frame(d,b,m+2);
    if(!e)e=status_ok(b);
    if(!e)memcpy(out,b+2,m);
    return e;
}
static int read_cmd(void *ctx, uint16_t op, uint8_t *out, size_t n)
{ return read_args(ctx,op,NULL,0,out,n); }
static int read32(void *ctx, uint32_t addr, uint32_t *v)
{
    uint8_t a[4]={(uint8_t)(addr>>16),(uint8_t)(addr>>8),(uint8_t)addr,1}, b[4];
    int e=read_args(ctx,0x0106,a,4,b,4);
    if(!e)*v=((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];
    return e;
}
static int write32(void *ctx, uint32_t addr, uint32_t v)
{
    const uint8_t a[11]={(uint8_t)(addr>>16),(uint8_t)(addr>>8),(uint8_t)addr,255,255,255,255,
        (uint8_t)(v>>24),(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v};
    return write_cmd(ctx,0x0105,a,sizeof(a));
}
static int standby(void *ctx)
{
    lr2021_iq_dev *d=ctx; uint8_t a=0;
    int e=write_cmd(ctx,0x0128,&a,1);
    if(!e)d->profile.receiving=false;
    return e;
}
static int receive(void *ctx)
{
    lr2021_iq_dev *d=ctx; const uint8_t a[3]={255,255,255};
    int e=write_cmd(ctx,0x020C,a,3);
    if(!e)d->profile.receiving=true;
    return e;
}
static int reset(void *ctx)
{
    lr2021_iq_dev *d=ctx;
    d->profile.configured=false; d->profile.receiving=false;
    int e=d->bus.reset(d->bus.context,true);
    d->bus.sleep_us(d->bus.context,5000);
    if(!e)e=d->bus.reset(d->bus.context,false);
    d->bus.sleep_us(d->bus.context,20000);
    if(e)return LR2021_IQ_IO;
    unsigned major=0,minor=0;
    e=lr2021_iq_setup(&d->hal,&d->board,&major,&minor);
    d->profile.fw_major=major; d->profile.fw_minor=minor; d->profile.lr2021=major==1;
    return e;
}
static int configure(lr2021_iq_dev *d, const lr2021_iq_rx *c)
{
    d->profile.configured=false;
    int e=lr2021_iq_board_rfsw(&d->hal,&d->board);
    if(!e)e=lr2021_iq_configure(&d->hal,c,&d->errors);
    if(!e) {
        d->rx=*c;
        lr2021_iq_profile *p=&d->profile;
        p->frequency_hz=c->frequency_hz; p->bandwidth_hz=c->bandwidth_hz;
        p->spreading_factor=7; p->exclusive_lora=true; p->configured=true;
        p->rx_path=(uint8_t)lr2021_iq_path(c->frequency_hz,c->path);
        p->gain_step=c->gain_step; p->rx_boost=c->boost;
        p->route=c->route; p->input_code=c->input_code; p->route_rate_hz=c->route?lr2021_iq_rx_rate(c):0;
    }
    return e;
}
static int restore(void *ctx, const lr2021_iq_profile *saved)
{
    lr2021_iq_dev *d=ctx;
    lr2021_iq_rx c=d->rx;
    /* A live gain change inside the session is carried into the restore. */
    c.gain_step=saved->gain_step; c.boost=saved->rx_boost;
    int e=configure(d,&c);
    if(!e && saved->receiving)e=receive(ctx);
    return e;
}
static void lock(void *ctx) { lr2021_iq_dev *d=ctx; if(d->bus.lock)d->bus.lock(d->bus.context); }
static void unlock(void *ctx) { lr2021_iq_dev *d=ctx; if(d->bus.unlock)d->bus.unlock(d->bus.context); }
static int snapshot(void *ctx, lr2021_iq_profile *p) { *p=((lr2021_iq_dev *)ctx)->profile; return 0; }
static uint64_t now(void *ctx) { lr2021_iq_dev *d=ctx; return d->bus.now_us(d->bus.context); }
static void sleep_us(void *ctx, unsigned us) { lr2021_iq_dev *d=ctx; d->bus.sleep_us(d->bus.context,us); }
static int fast(void *ctx, bool on)
{
    lr2021_iq_dev *d=ctx;
    if(on) {
        if(d->bus_owned)return LR2021_IQ_STATE;
        int e=d->bus.fast?d->bus.fast(d->bus.context,true):0;
        if(!e)d->bus_owned=true;
        return e?LR2021_IQ_IO:0;
    }
    if(d->bus_owned) {
        d->bus_owned=false;
        if(d->bus.fast && d->bus.fast(d->bus.context,false))return LR2021_IQ_IO;
    }
    return 0;
}
static int clear_fifo(void *ctx) { return write_cmd(ctx,0x011E,NULL,0); }
static int read_fifo(void *ctx, uint8_t *out, size_t n)
{
    if(!n || n>1020)return LR2021_IQ_ARGUMENT;
    uint8_t b[1022]={0,1};
    int e=frame(ctx,b,n+2);
    if(!e)e=status_ok(b);
    if(!e)memcpy(out,b+2,n);
    return e;
}
static int write_fifo(void *ctx, const uint8_t *in, size_t n)
{
    if(!n || n>1020)return LR2021_IQ_ARGUMENT; /* see lr2021_iq_hal */
    uint8_t b[1022]={0,2};
    memcpy(b+2,in,n);
    int e=frame(ctx,b,n+2);
    return e?e:status_ok(b);
}
int lr2021_iq_dev_open(lr2021_iq_dev *d, const lr2021_iq_bus *bus,
                       const lr2021_iq_board *board, const lr2021_iq_rx *rx)
{
    if(!d || !bus || !bus->transfer || !bus->busy || !bus->reset || !bus->now_us ||
       !bus->sleep_us || lr2021_iq_rx_check(rx))return LR2021_IQ_ARGUMENT;
    memset(d,0,sizeof(*d));
    d->bus=*bus;
    if(board)d->board=*board;
    d->rx=*rx;
    d->hal=(lr2021_iq_hal){.context=d,.lock=lock,.unlock=unlock,.snapshot=snapshot,
        .read32=read32,.write32=write32,.write=write_cmd,.read=read_cmd,.standby=standby,
        .receive=receive,.reset=reset,.restore=restore,.fast=fast,.clear_fifo=clear_fifo,
        .read_fifo=read_fifo,.now_us=now,.sleep_us=sleep_us,.write_fifo=write_fifo};
    lock(d);
    int e=reset(d);
    if(!e)e=configure(d,rx);
    unlock(d);
    return e;
}
int lr2021_iq_dev_configure(lr2021_iq_dev *d, const lr2021_iq_rx *rx)
{
    if(!d || lr2021_iq_rx_check(rx))return LR2021_IQ_ARGUMENT;
    lock(d);
    bool was=d->profile.receiving;
    int e=configure(d,rx);
    if(!e && was)e=receive(d);
    unlock(d);
    return e;
}
