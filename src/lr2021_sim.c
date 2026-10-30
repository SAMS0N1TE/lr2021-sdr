/* SPDX-License-Identifier: MIT */
#include "lr2021_sim.h"
#include <math.h>
#include <string.h>

enum { OK=2, PERR=1, FAIL=0, DAT=3, STBY=1, RX=4, TX=5 };
#define RX_DEFAULT_BASE 0x6140u
#define RX_DEFAULT_SIZE 0xE4u

uint64_t lr2021_sim_now_us(lr2021_sim *s)
{ return s->clock ? s->clock(s->clock_context) : s->vtime; }
static void chip_reset(lr2021_sim *s)
{
    s->mode=STBY; s->packet_type=0; s->route=0; s->errors=0; s->irq=0;
    s->rx_base=RX_DEFAULT_BASE; s->rx_size=RX_DEFAULT_SIZE; s->tx_base=0x6000; s->tx_size=0xE4;
    s->rx_level=s->tx_level=s->tx_head=0; s->rx_flags=s->tx_flags=0;
    s->have_resp=false; s->cmd_status=OK; s->tx_n=0; s->path=s->boost=s->step=0;
    s->reset_seen=true; s->resets++;
}
void lr2021_sim_init(lr2021_sim *s)
{
    memset(s,0,sizeof(*s));
    s->tone_amplitude=4000; s->noise=0x2545F491u;
    chip_reset(s); s->resets=0;
}
static unsigned rx_capacity(const lr2021_sim *s)
{ return (s->rx_base&0x7FFFFF)==0x4400 && s->rx_size==0x3E8 ? 1024u : 256u; }
static unsigned tx_capacity(const lr2021_sim *s)
{ return (s->tx_base&0x7FFFFF)==0x4000 && s->tx_size==0x3FC ? 1024u : 256u; }
static bool raw_route(const lr2021_sim *s)
{ return s->packet_type==2 && (s->route==0x08 || s->route==0x09 || s->route==0x0E); }
static unsigned rate(const lr2021_sim *s)
{
    static const unsigned r[]={0,0,125000,250000,500000,1000000};
    unsigned v=raw_route(s) ? s->route_rate : s->packet_type==0 && s->route==0x0A && s->lora_bw<6 ? r[s->lora_bw] : 0;
    return s->rate_percent ? (unsigned)((uint64_t)v*s->rate_percent/100u) : v;
}
static int16_t noise(lr2021_sim *s)
{
    uint32_t x=s->noise; x^=x<<13; x^=x>>17; x^=x<<5; s->noise=x;
    return (int16_t)((int)(x&31)-16);
}
static double input_amplitude(const lr2021_sim *s)
{
    if(s->pattern!=LR2021_SIM_TONE || s->mode!=RX || !rate(s))return 0;
    double offset=(double)s->tone_hz-(double)s->freq;
    bool heard=s->freq>=100000000u && s->freq<=2740000000u && fabs(offset)<rate(s)/2.0;
    return heard?s->tone_amplitude:0;
}
static unsigned agc_step(const lr2021_sim *s)
{
    if(s->step)return s->step;
    double a=input_amplitude(s);
    unsigned g=13;
    while(g>1 && a*pow(10.0,((double)g-13)/20.0)>8192.0)g--;
    return g;
}
static uint32_t agc_value(const lr2021_sim *s)
{
    static const uint16_t v[13]={16,28,38,49,57,64,73,85,96,108,120,127,138};
    return !s->step && input_amplitude(s)==0 ? 139u : v[agc_step(s)-1];
}
static uint32_t rssi_field(const lr2021_sim *s)
{
    double a=input_amplitude(s), dbm=-120.0+(a>16.0?20.0*log10(a/16.0):0.0);
    long v=lround(-2.0*dbm);
    return (uint32_t)(v<0?0:v>511?511:v);
}
static void produce(lr2021_sim *s, uint64_t pair)
{
    int16_t i,q;
    if(s->pattern==LR2021_SIM_COUNTER) { i=(int16_t)(uint16_t)pair; q=(int16_t)(uint16_t)(pair>>16); }
    else {
        double gain=pow(10.0,((double)agc_step(s)-13+(s->step>=12?s->boost*0.2:0))/20.0);
        double offset=(double)s->tone_hz-(double)s->freq, a=s->tone_amplitude*gain;
        bool heard=s->freq>=100000000u && s->freq<=2740000000u && fabs(offset)<rate(s)/2.0;
        if(!heard)a=0;
        s->phase+=6.283185307179586*offset/rate(s);
        if(s->phase>6.283185307179586)s->phase-=6.283185307179586;
        if(s->phase<-6.283185307179586)s->phase+=6.283185307179586;
        i=(int16_t)(a*cos(s->phase))+noise(s); q=(int16_t)(a*sin(s->phase))+noise(s);
    }
    unsigned cap=rx_capacity(s), size=raw_route(s)?2:4;
    if(s->overflow_after && pair>=s->overflow_after)s->rx_flags|=0x10;
    if(s->rx_level+size>cap || (s->rx_flags&0x10)) { s->rx_flags|=0x10; return; }
    uint8_t *p=s->rx_fifo+s->rx_level;
    if(size==2) {
        if(s->pattern==LR2021_SIM_COUNTER) { p[0]=(uint8_t)pair; p[1]=(uint8_t)(pair>>8); }
        else if(s->route==0x0E) { /* IQ6 in the high bits, gain step in the low bits */
            unsigned g=agc_step(s);
            p[0]=(uint8_t)(((i>>10)*4)|(g&3)); p[1]=(uint8_t)(((q>>10)*4)|((g>>2)&3));
        } else { p[0]=(uint8_t)(i>>8); p[1]=(uint8_t)(q>>8); }
    } else {
        p[0]=(uint8_t)i; p[1]=(uint8_t)((uint16_t)i>>8); p[2]=(uint8_t)q; p[3]=(uint8_t)((uint16_t)q>>8);
    }
    s->rx_level+=size;
}
static void advance(lr2021_sim *s)
{
    uint64_t now=lr2021_sim_now_us(s);
    if(s->mode==RX && rate(s)) {
        uint64_t target=s->rx_origin+(now-s->rx_t0)*rate(s)/1000000u;
        /* Bound the catch-up after a long host stall to one overfull FIFO. */
        if(target>s->rx_pairs+300) {
            if(s->stall_tolerant){ s->rx_origin+=s->rx_pairs+256-target; target=s->rx_pairs+256; }
            else { s->rx_flags|=0x10; s->rx_pairs=target-300; }
        }
        while(s->rx_pairs<target) {
            if(s->stall_tolerant && s->rx_level+8>=rx_capacity(s)) {
                s->rx_origin-=target-s->rx_pairs; /* pause instead of overflowing */
                break;
            }
            produce(s,s->rx_pairs++);
        }
    }
    if(s->mode==TX && s->tx_n) {
        uint64_t target=(now-s->tx_t0)*s->tx_rate/1000000u;
        if(target>s->tx_n)target=s->tx_n;
        while(s->tx_consumed<target) {
            if(s->tx_level<2) { s->tx_flags|=0x20; s->tx_consumed=target; break; }
            for(unsigned k=0;k<2;k++) {
                uint8_t b=s->tx_fifo[s->tx_head];
                s->tx_head=(s->tx_head+1)%1024; s->tx_level--;
                if(s->tx_capture && s->tx_captured<s->tx_capacity)s->tx_capture[s->tx_captured++]=b;
            }
            s->tx_consumed++;
        }
        if(s->tx_consumed>=s->tx_n) { s->irq|=1u<<19; s->mode=STBY; }
    }
}
static void respond(lr2021_sim *s, const uint8_t *p, size_t n)
{ memcpy(s->resp,p,n); s->resp_len=n; s->have_resp=true; }
static uint8_t execute(lr2021_sim *s, uint16_t op, const uint8_t *a, size_t n)
{
    uint8_t r[8]={0};
    bool idle=s->mode==STBY;
    switch(op) {
    case 0x0101: r[0]=1; r[1]=24; respond(s,r,2); return OK;
    case 0x0110: r[0]=(uint8_t)(s->errors>>8); r[1]=(uint8_t)s->errors; respond(s,r,2); return OK;
    case 0x0111: s->errors=0; return OK;
    case 0x0112: case 0x0113: return n==2 && a[0]>=5 && a[0]<=11 ? OK : PERR;
    case 0x0114: if(n!=2)return PERR; s->rx_flags&=(uint8_t)~a[0]; s->tx_flags&=(uint8_t)~a[1]; return OK;
    case 0x0116: if(n!=4)return PERR; s->irq&=~(((uint32_t)a[0]<<24)|((uint32_t)a[1]<<16)|((uint32_t)a[2]<<8)|a[3]); return OK;
    case 0x0117: r[0]=(uint8_t)(s->irq>>24); r[1]=(uint8_t)(s->irq>>16); r[2]=(uint8_t)(s->irq>>8); r[3]=(uint8_t)s->irq;
        s->irq=0; respond(s,r,4); return OK;
    case 0x011B: r[0]=s->rx_flags; r[1]=s->tx_flags; respond(s,r,2); return OK;
    case 0x011C: {
        unsigned l=(s->rx_flags&0x10)?rx_capacity(s)+4:s->rx_level;
        r[0]=(uint8_t)(l>>8); r[1]=(uint8_t)l; respond(s,r,2); return OK;
    }
    case 0x011D: r[0]=(uint8_t)(s->tx_level>>8); r[1]=(uint8_t)s->tx_level; respond(s,r,2); return OK;
    case 0x011E: s->rx_level=0; return OK;
    case 0x011F: s->tx_level=s->tx_head=0; return OK;
    case 0x0120: return n==5 && a[0]<=7 && idle ? OK : FAIL;
    case 0x0121: case 0x0122: case 0x0206: return n==1 ? OK : PERR;
    case 0x0123: return n==6 && idle ? OK : FAIL;
    case 0x0128: s->mode=STBY; return OK;
    case 0x0105: {
        if(n!=11)return PERR;
        uint32_t addr=((uint32_t)a[0]<<16)|((uint32_t)a[1]<<8)|a[2];
        uint32_t v=((uint32_t)a[7]<<24)|((uint32_t)a[8]<<16)|((uint32_t)a[9]<<8)|a[10];
        if(addr==0xF30028)s->rx_base=v&0x7FFF; /* ROM 1.24 drops the alias bit */
        else if(addr==0xF30030)s->rx_size=v;
        else if(addr==0xF3002C)s->tx_base=v&0x7FFF;
        else if(addr==0xF30034)s->tx_size=v;
        return OK;
    }
    case 0x0106: {
        if(n!=4 || a[3]!=1)return PERR;
        uint32_t addr=((uint32_t)a[0]<<16)|((uint32_t)a[1]<<8)|a[2], v=0;
        if(addr==0xF30028)v=s->rx_base; else if(addr==0xF30030)v=s->rx_size;
        else if(addr==0xF3002C)v=s->tx_base; else if(addr==0xF30034)v=s->tx_size;
        else if(addr==0xF30154)v=((uint32_t)(s->mode==RX?agc_step(s):10u)<<20)|((uint32_t)s->step<<16);
        else if(addr==0xF3012C)v=(s->mode==RX?agc_value(s):0x1FFu)<<18;
        else if(addr==0xF301A8)v=(s->mode==RX?rssi_field(s):0x1FFu)<<18;
        r[0]=(uint8_t)(v>>24); r[1]=(uint8_t)(v>>16); r[2]=(uint8_t)(v>>8); r[3]=(uint8_t)v;
        respond(s,r,4); return OK;
    }
    case 0x0200:
        if(n!=4)return PERR;
        s->freq=((uint32_t)a[0]<<24)|((uint32_t)a[1]<<16)|((uint32_t)a[2]<<8)|a[3];
        if(s->freq<104000000u)s->errors|=LR2021_IQ_ERR_IMG_CAL;
        if(s->freq>2745000000u)s->errors|=LR2021_IQ_ERR_PLL_LOCK|LR2021_IQ_ERR_IMG_CAL;
        return OK;
    case 0x0201:
        if(n!=2 || a[0]>1 || a[1]>7)return PERR;
        if(s->mode==RX)return FAIL; /* measured on ROM 1.24: refused during RX */
        s->path=a[0]; s->boost=a[1]; s->gain_writes++; return OK;
    case 0x0202: return n==3 ? OK : PERR;
    case 0x0203: return n==2 && (int8_t)a[0]>=-19 && (int8_t)a[0]<=44 ? OK : PERR;
    case 0x0207: if(n!=1 || !idle)return FAIL; s->packet_type=a[0]; return OK;
    case 0x020C:
        if(n!=3 || (s->packet_type!=0 && s->packet_type!=2) || (s->errors&LR2021_IQ_ERR_PLL_LOCK))return FAIL;
        s->mode=RX; s->rx_t0=lr2021_sim_now_us(s); s->rx_origin=s->rx_pairs; s->rx_starts++; return OK;
    case 0x020D:
        if(n!=3 || s->packet_type!=0x0A || !s->tx_n || s->tx_selector)return FAIL;
        s->mode=TX; s->tx_t0=lr2021_sim_now_us(s); s->tx_consumed=0; s->tx_starts++; return OK;
    case 0x021A: if(n!=1 || a[0]>13)return PERR; s->step=a[0]; s->gain_writes++; return OK;
    case 0x0220: if(n!=2 || s->packet_type!=0)return FAIL; s->lora_bw=a[0]&15; return OK;
    case 0x0221: case 0x0223: return s->packet_type==0 ? OK : FAIL;
    case 0x0281: return n==7 && s->packet_type==0x0A ? OK : FAIL;
    case 0x0282: return n==6 && s->packet_type==0x0A ? OK : FAIL;
    case 0x0284: return n==5 && s->packet_type==0x0A ? OK : FAIL;
    case 0x0290:
        if(n!=11 || !idle)return FAIL;
        s->route=a[5]; s->route_code=a[0];
        s->route_rate=((uint32_t)a[1]<<24)|((uint32_t)a[2]<<16)|((uint32_t)a[3]<<8)|a[4];
        return OK;
    case 0x0240: return n==9 && s->packet_type==2 ? OK : FAIL;
    case 0x0293:
        if(n!=7 || s->packet_type!=0x0A || !idle)return FAIL;
        s->tx_n=((unsigned)a[0]<<8)|a[1];
        s->tx_rate=((uint32_t)a[2]<<24)|((uint32_t)a[3]<<16)|((uint32_t)a[4]<<8)|a[5];
        s->tx_selector=a[6];
        return s->tx_n && s->tx_rate ? OK : PERR;
    default: return PERR;
    }
}
static int transfer(void *ctx, uint8_t *b, size_t n)
{
    lr2021_sim *s=ctx;
    if(s->in_reset || n<2)return -1;
    if(!s->clock)s->vtime+=2+n/2;
    advance(s);
    s->spi_frames++;
    uint8_t in[1030];
    if(n>sizeof(in))return -1;
    memcpy(in,b,n);
    unsigned st=((unsigned)(s->have_resp?DAT:s->cmd_status)<<9)|(s->irq?0x100u:0)|
        (s->reset_seen?0x20u:0)|s->mode;
    memset(b,0,n);
    b[0]=(uint8_t)(st>>8); b[1]=(uint8_t)st;
    if(s->have_resp) { /* response frame of the previous read command */
        memcpy(b+2,s->resp,s->resp_len<n-2?s->resp_len:n-2);
        s->have_resp=false; s->cmd_status=OK; return 0;
    }
    uint16_t op=(uint16_t)(in[0]<<8|in[1]);
    if(op==0x0100) { /* GetStatus: status above plus IRQs; clears reset source */
        if(n>=6){b[2]=(uint8_t)(s->irq>>24);b[3]=(uint8_t)(s->irq>>16);b[4]=(uint8_t)(s->irq>>8);b[5]=(uint8_t)s->irq;}
        s->reset_seen=false; return 0;
    }
    if(op==0x0001) {
        size_t m=n-2;
        if(m>s->rx_level){ s->rx_flags|=0x20; m=s->rx_level; }
        memcpy(b+2,s->rx_fifo,m);
        memmove(s->rx_fifo,s->rx_fifo+m,s->rx_level-m); s->rx_level-=(unsigned)m;
        return 0;
    }
    if(op==0x0002) {
        for(size_t i=2;i<n;i++) {
            if(s->tx_level>=tx_capacity(s)){ s->tx_flags|=0x10; break; }
            s->tx_fifo[(s->tx_head+s->tx_level)%1024]=in[i]; s->tx_level++;
        }
        return 0;
    }
    s->cmd_status=execute(s,op,in+2,n-2);
    return 0;
}
static int busy(void *ctx) { lr2021_sim *s=ctx; if(!s->clock)s->vtime++; return s->in_reset?1:0; }
static int reset(void *ctx, bool on)
{
    lr2021_sim *s=ctx;
    if(on && !s->in_reset)chip_reset(s);
    s->in_reset=on; return 0;
}
static uint64_t now(void *ctx) { lr2021_sim *s=ctx; if(!s->clock)s->vtime++; return lr2021_sim_now_us(s); }
static void sleep_us(void *ctx, unsigned us)
{
    lr2021_sim *s=ctx;
    if(s->clock){ if(s->sleep)s->sleep(s->clock_context,us); }
    else s->vtime+=us;
}
lr2021_iq_bus lr2021_sim_bus(lr2021_sim *s)
{ return (lr2021_iq_bus){.context=s,.transfer=transfer,.busy=busy,.reset=reset,.now_us=now,.sleep_us=sleep_us}; }
