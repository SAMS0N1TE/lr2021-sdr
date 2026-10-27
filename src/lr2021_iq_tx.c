/* SPDX-License-Identifier: GPL-3.0-only */
#include "lr2021_iq_tx.h"
#include <string.h>

#define TX_FIFO 1024u
#define TX_PREFILL 896u
#define TX_REFILL_AT 768u
/* Measured on the LilyGO T-Display-P4: TX FIFO frames with more than 62 data
 * bytes lose data, and the burst then underflows without TX_DONE. */
#define TX_WRITE 62u
#define IRQ_TX_DONE (1u<<19)
#define IRQ_TIMEOUT (1u<<21)

bool lr2021_iq_tx_rate_ok(uint32_t r)
{ return r==4800 || r==9600 || r==19200 || r==38400 || r==76800 || r==125000; }
unsigned lr2021_iq_tx_pa(const lr2021_iq_tx *t)
{ return t->pa==LR2021_IQ_PA_AUTO ? (t->frequency_hz>=LR2021_IQ_HF_HZ ? 1u : 0u) : t->pa; }
uint32_t lr2021_iq_tx_untested(const lr2021_iq_tx *t)
{
    uint32_t u=0;
    uint64_t n=(uint64_t)t->pairs*t->repeats;
    if(!lr2021_iq_tx_rate_ok(t->rate_hz))u|=LR2021_IQ_TX_UNTESTED_RATE;
    if(t->repeats>LR2021_IQ_TX_MAX_REPEATS)u|=LR2021_IQ_TX_UNTESTED_REPEATS;
    if(!t->rate_hz || n*1000000u>(uint64_t)LR2021_IQ_TX_MAX_US*t->rate_hz)u|=LR2021_IQ_TX_UNTESTED_DURATION;
    if(t->frequency_hz<LR2021_IQ_TX_MIN_HZ || t->frequency_hz>LR2021_IQ_TX_MAX_HZ)u|=LR2021_IQ_TX_UNTESTED_FREQUENCY;
    if(lr2021_iq_tx_pa(t))u|=LR2021_IQ_TX_UNTESTED_HF_PA;
    if(t->power_half_db<LR2021_IQ_TX_MIN_POWER || t->power_half_db>LR2021_IQ_TX_MAX_POWER)u|=LR2021_IQ_TX_UNTESTED_POWER;
    for(size_t i=0;t->samples && i<t->pairs;i++) {
        int a=t->samples[2*i],b=t->samples[2*i+1];
        if(a*a+b*b>LR2021_IQ_TX_MAX_RADIUS*LR2021_IQ_TX_MAX_RADIUS){ u|=LR2021_IQ_TX_UNTESTED_RADIUS; break; }
    }
    return u;
}
int lr2021_iq_tx_check(const lr2021_iq_tx *t)
{
    if(!t || !t->samples || !t->pairs || !t->repeats || t->repeats>255 || !t->rate_hz ||
       t->rate_hz>1000000u || t->pa>LR2021_IQ_PA_AUTO || t->frequency_hz<LR2021_IQ_MIN_HZ ||
       t->frequency_hz>LR2021_IQ_MAX_HZ)return LR2021_IQ_ARGUMENT;
    uint64_t n=(uint64_t)t->pairs*t->repeats;
    bool hf=lr2021_iq_tx_pa(t);
    if(n>LR2021_IQ_TX_MAX_SAMPLES || t->power_half_db<(hf?-39:-19) || t->power_half_db>(hf?24:44) ||
       n*1000000u>500000000ull*t->rate_hz)return LR2021_IQ_ARGUMENT;
    if((t->flags&LR2021_IQ_TX_CHECKED) && lr2021_iq_tx_untested(t))return LR2021_IQ_ARGUMENT;
    return 0;
}
/* Queue a bounded slice of the repeated buffer; positions are bytes. */
static int enqueue(const lr2021_iq_hal *h, const uint8_t *frame, size_t frame_len,
                   size_t total, size_t *queued, size_t limit, unsigned *writes)
{
    if(limit>total-*queued)limit=total-*queued;
    while(limit) {
        size_t offset=*queued%frame_len, n=frame_len-offset;
        if(n>limit)n=limit;
        if(n>TX_WRITE)n=TX_WRITE;
        int e=h->write_fifo(h->context,frame+offset,n);
        if(e)return e;
        *queued+=n; limit-=n; ++*writes;
    }
    return 0;
}
static int put(const lr2021_iq_hal *h, uint16_t op, const uint8_t *a, size_t n)
{ return h->write(h->context,op,a,n); }
static int setup(const lr2021_iq_hal *h, const lr2021_iq_board *b, const lr2021_iq_tx *t, unsigned n)
{
    const uint32_t hz=t->frequency_hz, rate=t->rate_hz;
    const unsigned bytes=2*n;
    const uint8_t type=0x0A; /* OOK */
    const uint8_t freq[4]={(uint8_t)(hz>>24),(uint8_t)(hz>>16),(uint8_t)(hz>>8),(uint8_t)hz};
    /* SetOokModulationParams: bitrate = sample rate, no shaping, 111.1 kHz RX
     * filter (code 226, the first entry above the recipe's 100 kHz), full depth. */
    const uint8_t mod[7]={(uint8_t)(rate>>24),(uint8_t)(rate>>16),(uint8_t)(rate>>8),(uint8_t)rate,0,226,0};
    /* Fixed length, 32-symbol preamble, no address, CRC or Manchester. */
    const uint8_t pkt[6]={0,32,0,(uint8_t)(bytes>>8),(uint8_t)bytes,0};
    const uint8_t sync[5]={0,0,0,0,0};
    /* LF PA, full single-ended; duty 1 / slices 2 is the lowest-power row of the
     * datasheet 915 MHz table. HF PA: LF fields at their unused values (6, 7),
     * HF duty 28 from the 0 dBm row of the 2445 MHz table. Ramp code 4 is 32 us. */
    const bool hf=lr2021_iq_tx_pa(t);
    const uint8_t pa[3]={(uint8_t)(hf?0x80:0x00),(uint8_t)(hf?0x67:0x12),(uint8_t)(hf?28:16)};
    const uint8_t power[2]={(uint8_t)(int8_t)t->power_half_db,4}, fallback=1;
    const uint8_t cmd[7]={(uint8_t)(n>>8),(uint8_t)n,(uint8_t)(rate>>24),(uint8_t)(rate>>16),
        (uint8_t)(rate>>8),(uint8_t)rate,0};
    const uint8_t simo=2;
    int e=lr2021_iq_board_rfsw(h,b);
    if(!e && (t->flags&LR2021_IQ_TX_SIMO))e=put(h,0x0121,&simo,1); /* in standby RC */
    if(!e)e=put(h,0x0111,NULL,0);
    if(!e)e=put(h,0x0207,&type,1);
    if(!e)e=put(h,0x0200,freq,4);
    if(!e)e=put(h,0x0281,mod,7);
    if(!e)e=put(h,0x0282,pkt,6);
    if(!e)e=put(h,0x0284,sync,5);
    if(!e)e=put(h,0x0202,pa,3);
    if(!e)e=put(h,0x0203,power,2);
    if(!e)e=put(h,0x0206,&fallback,1);
    /* Extended TX FIFO: base 0x804000, size encoding 0x3FC. */
    uint32_t base=0,size=0;
    if(!e)e=h->write32(h->context,0xF3002C,0x804000);
    if(!e)e=h->write32(h->context,0xF30034,0x3FC);
    if(!e)e=h->read32(h->context,0xF3002C,&base);
    if(!e)e=h->read32(h->context,0xF30034,&size);
    if(!e && ((base!=0x804000 && base!=0x4000) || size!=0x3FC))e=LR2021_IQ_DATA;
    if(!e)e=put(h,0x0293,cmd,7);
    return e;
}
int lr2021_iq_transmit(const lr2021_iq_hal *h, const lr2021_iq_board *b,
                      const lr2021_iq_tx *t, lr2021_iq_tx_result *r)
{
    if(!r)return LR2021_IQ_ARGUMENT;
    memset(r,0,sizeof(*r)); r->error=LR2021_IQ_ARGUMENT; r->min_level=TX_FIFO;
    if(!h || !h->lock || !h->unlock || !h->snapshot || !h->read32 || !h->write32 ||
       !h->write || !h->read || !h->standby || !h->reset || !h->restore || !h->fast ||
       !h->write_fifo || !h->now_us || !h->sleep_us || lr2021_iq_tx_check(t))return LR2021_IQ_ARGUMENT;
    r->untested=lr2021_iq_tx_untested(t);
    const unsigned n=(unsigned)(t->pairs*t->repeats);
    const size_t frame_len=2*t->pairs, total=2*(size_t)n;
    const uint64_t duration=((uint64_t)n*1000000u+t->rate_hz-1)/t->rate_hz;
    const uint64_t timeout=duration+50000;
    const uint8_t *frame=(const uint8_t *)t->samples;
    void *c=h->context;
    h->lock(c);
    lr2021_iq_profile p={0};
    uint32_t marker=0;
    int e=h->snapshot(c,&p);
    if(!e && (!p.lr2021 || p.fw_major!=1 || p.fw_minor!=24 || !p.configured || !p.exclusive_lora))
        e=LR2021_IQ_STATE;
    if(!e && t->start_us && t->start_us>h->now_us(c)+10000000u)e=LR2021_IQ_ARGUMENT;
    if(!e)e=h->read32(c,0x800FF8,&marker);
    if(!e && marker==0x600DB002)e=LR2021_IQ_STATE;
    bool touched=!e, fast=false, sent=false;
    int stop=0;
    if(!e)e=h->standby(c);
    if(!e)e=setup(h,b,t,n);
    if(!e){e=h->fast(c,true);fast=!e;}
    const uint8_t clear[2]={0x3F,0x3F}, all[4]={0xFF,0xFF,0xFF,0xFF};
    if(!e)e=put(h,0x011F,NULL,0);
    if(!e)e=put(h,0x0116,all,4);
    if(!e)e=put(h,0x0114,clear,2);
    unsigned writes=0;
    if(!e)e=enqueue(h,frame,frame_len,total,&r->queued_bytes,TX_PREFILL,&writes);
    while(!e && t->start_us && h->now_us(c)<t->start_us) {
        uint64_t left=t->start_us-h->now_us(c);
        h->sleep_us(c,left>1000?1000u:(unsigned)left);
    }
    uint8_t flags[2]={0}, level[2]={0}, irq[4]={0};
    uint64_t start=h->now_us(c), previous=start;
    if(!e) {
        const uint32_t ticks=(uint32_t)((timeout*32+999)/1000);
        const uint8_t a[3]={(uint8_t)(ticks>>16),(uint8_t)(ticks>>8),(uint8_t)ticks};
        start=h->now_us(c); previous=start; r->started_us=start;
        e=put(h,0x020D,a,3);
        sent=!e;
    }
    while(!e && h->now_us(c)-start<timeout) {
        uint64_t now=h->now_us(c);
        if(now-previous>r->max_service_gap_us)r->max_service_gap_us=now-previous;
        previous=now;
        if(r->queued_bytes<total) {
            e=h->read(c,0x011D,level,2);
            unsigned fill=((unsigned)level[0]<<8)|level[1];
            if(!e && fill>TX_FIFO)e=LR2021_IQ_DATA;
            if(!e && fill<r->min_level)r->min_level=fill;
            if(!e && fill<=TX_REFILL_AT)
                e=enqueue(h,frame,frame_len,total,&r->queued_bytes,(TX_PREFILL-fill)&~1u,&r->refills);
            if(e)break;
        }
        e=h->read(c,0x011B,flags,2);
        r->tx_flags|=flags[1];
        if(!e && (flags[1]&0x30))e=LR2021_IQ_DATA;
        if(!e)e=h->read(c,0x0117,irq,4);
        if(!e) {
            r->irq|=((uint32_t)irq[0]<<24)|((uint32_t)irq[1]<<16)|((uint32_t)irq[2]<<8)|irq[3];
            if(r->irq&(IRQ_TX_DONE|IRQ_TIMEOUT))break;
        }
        /* Refills of up to 896 bytes leave >= 3 ms at 125 ksample/s. */
        if(r->queued_bytes<total)h->sleep_us(c,250);
        else h->sleep_us(c,1000);
    }
    r->elapsed_us=h->now_us(c)-start;
    if(sent) {
        int f=h->read(c,0x011B,flags,2);
        if(!f){r->tx_flags|=flags[1]; f=h->read(c,0x011D,level,2);}
        if(!f)r->final_level=((unsigned)level[0]<<8)|level[1];
        if(!e)e=f;
        if(!e && (!(r->irq&IRQ_TX_DONE) || (r->irq&IRQ_TIMEOUT) || r->queued_bytes!=total))
            e=LR2021_IQ_TIMEOUT;
        if(!e && (r->tx_flags&0x30))e=LR2021_IQ_DATA;
    }
    /* Always stop locally, then recover with reset and the saved RX profile. */
    if(touched)stop=h->standby(c);
    int bus=fast?h->fast(c,false):0;
    if(touched) {
        r->recovery_error=h->reset(c);
        if(!r->recovery_error)r->recovery_error=h->restore(c,&p);
        if(!r->recovery_error)r->recovery_error=bus;
    }
    r->stop_error=stop; r->error=e;
    h->unlock(c);
    return e?e:stop?stop:r->recovery_error;
}
