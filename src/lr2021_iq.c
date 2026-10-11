#include "lr2021_iq.h"
#include <string.h>

static bool valid_hal(const lr2021_iq_hal *h)
{
    return h && h->lock && h->unlock && h->snapshot && h->read32 &&
        h->write32 && h->write && h->read && h->standby && h->receive &&
        h->reset && h->restore && h->now_us && h->sleep_us;
}
static int preflight(const lr2021_iq_hal *h, lr2021_iq_profile *p)
{
    uint32_t magic=0;
    int e=h->snapshot(h->context,p);
    if(e)return e;
    if(!p->lr2021 || p->fw_major!=1 || p->fw_minor!=24 ||
       !p->configured || !p->exclusive_lora)return LR2021_IQ_STATE;
    e=h->read32(h->context,0x800FF8,&magic);
    return e ? e : magic==0x600DB002 ? LR2021_IQ_STATE : 0;
}
static int outcome(lr2021_iq_result *r)
{
    return r->capture_error ? r->capture_error :
           r->restore_error ? r->restore_error : r->recovery_error;
}
int lr2021_iq_capture(const lr2021_iq_hal *h, uint32_t *words, uint32_t *scratch,
                     size_t capacity, lr2021_iq_result *r)
{
    if(!r)return LR2021_IQ_ARGUMENT;
    memset(r,0,sizeof(*r)); r->capture_error=LR2021_IQ_ARGUMENT;
    uintptr_t a=(uintptr_t)words,b=(uintptr_t)scratch;
    if(!valid_hal(h) || !words || !scratch || capacity<256 ||
       (a>b?a-b:b-a)<1024)return LR2021_IQ_ARGUMENT;
    void *c=h->context;
    h->lock(c);
    uint64_t start=h->now_us(c);
    lr2021_iq_profile p={0};
    int e=preflight(h,&p);
    for(unsigned i=0;!e && i<256;i++)e=h->read32(c,0x802000+4*i,&scratch[i]);
    bool touched=!e;
    if(!e)e=h->standby(c);
    const uint8_t args[]={0xFF,0,0,0,0,0x92,0,0,0,0xFF,0};
    if(!e)e=h->write(c,0x0290,args,sizeof(args));
    if(!e)e=h->receive(c);
    if(!e) {
        uint64_t began=h->now_us(c);
        do {
            e=h->read32(c,0xF30054,&r->status);
            if(e || (r->status&0x03000000)==0x03000000)break;
            h->sleep_us(c,1000);
        } while(h->now_us(c)-began<100000);
        if(!e && (r->status&0x03000000)!=0x03000000)e=LR2021_IQ_TIMEOUT;
    }
    if(!e)e=h->standby(c);
    if(!e) {
        uint8_t n[2]={0}; e=h->read(c,0x0291,n,2);
        if(!e && (((unsigned)n[0]<<8)|n[1])!=1024)e=LR2021_IQ_DATA;
    }
    for(unsigned i=0;!e && i<256;i++) {
        e=h->read32(c,0x802000+4*i,&words[i]);
        if(!e)r->bytes+=4;
    }
    if(touched) {
        r->recovery_error=h->reset(c);
        if(!r->recovery_error) {
            for(unsigned i=0;!r->restore_error && i<256;i++)
                r->restore_error=h->write32(c,0x802000+4*i,scratch[i]);
            for(unsigned i=0;!r->restore_error && i<256;i++) {
                uint32_t v=0;
                r->restore_error=h->read32(c,0x802000+4*i,&v);
                if(!r->restore_error && v!=scratch[i])r->restore_error=LR2021_IQ_DATA;
            }
            r->recovery_error=h->restore(c,&p);
        } else r->restore_error=LR2021_IQ_STATE;
    }
    if(p.frequency_hz==910525000 && p.spreading_factor==7 &&
       (p.bandwidth_hz==31250 || p.bandwidth_hz==62500 ||
        p.bandwidth_hz==125000 || p.bandwidth_hz==250000))
        r->nominal_rate_hz=4*p.bandwidth_hz;
    r->elapsed_us=h->now_us(c)-start; r->capture_error=e;
    h->unlock(c); return outcome(r);
}
int lr2021_iq_stream(const lr2021_iq_hal *h, unsigned duration,
                    lr2021_iq_sink sink, void *context, lr2021_iq_result *r)
{
    if(!r)return LR2021_IQ_ARGUMENT;
    memset(r,0,sizeof(*r)); r->capture_error=LR2021_IQ_ARGUMENT;
    if(!valid_hal(h) || !h->fast || !h->clear_fifo || !h->read_fifo ||
       !sink || !duration || duration>250)return LR2021_IQ_ARGUMENT;
    void *c=h->context;
    h->lock(c);
    lr2021_iq_profile p={0}; int e=preflight(h,&p);
    if(!e && p.bandwidth_hz!=31250)e=LR2021_IQ_STATE;
    bool touched=!e, fast=false;
    if(!e){e=h->fast(c,true);fast=!e;}
    if(!e)e=h->standby(c);
    const uint8_t args[]={0xFF,0,0,0,0,0x0A,0,0,0,0,1};
    const uint8_t clear[]={0x3F,0x3F};
    if(!e)e=h->write(c,0x0290,args,sizeof(args));
    if(!e)e=h->write(c,0x0114,clear,2);
    if(!e)e=h->receive(c);
    if(!e)e=h->clear_fifo(c);
    if(!e)e=h->write(c,0x0114,clear,2);
    uint64_t start=h->now_us(c);
    while(!e && h->now_us(c)-start<(uint64_t)duration*1000) {
        uint8_t level_bytes[2]={0};
        e=h->read(c,0x011C,level_bytes,2); r->polls++;
        unsigned level=((unsigned)level_bytes[0]<<8)|level_bytes[1];
        if(level>r->max_fifo_level)r->max_fifo_level=level;
        if(!e && level>=256)e=LR2021_IQ_DATA;
        size_t n=level&~3u;
        if(!e && n) {
            uint8_t data[256]; e=h->read_fifo(c,data,n);
            if(!e) {
                uint64_t first=r->bytes/4; r->bytes+=n;
                if(!sink(context,data,n,first)){r->stopped_by_sink=true;break;}
            }
        }
    }
    r->elapsed_us=h->now_us(c)-start;
    if(touched) {
        uint8_t flags[2]={0}; int f=h->read(c,0x011B,flags,2);
        r->fifo_flags=flags[0];
        int stop=h->standby(c);
        if(!e)e=stop?stop:f;
        if(!e && (flags[0]&0x30))e=LR2021_IQ_DATA;
        if(!e && !r->bytes)e=LR2021_IQ_TIMEOUT;
        int bus=fast?h->fast(c,false):0;
        r->recovery_error=h->reset(c);
        if(!r->recovery_error)r->recovery_error=h->restore(c,&p);
        if(!r->recovery_error)r->recovery_error=bus;
    }
    r->capture_error=e; r->nominal_rate_hz=125000;
    h->unlock(c); return outcome(r);
}
