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
static int verify_fifo(const lr2021_iq_hal *h)
{
    uint32_t base=0,size=0;
    int e=h->read32(h->context,0xF30028,&base);
    if(!e)e=h->read32(h->context,0xF30030,&size);
    /* Tested ROM 1.24 drops the RAM alias bit when reading the base back. */
    return e?e:((base==0x804400 || base==0x4400) && size==0x3E8?0:LR2021_IQ_DATA);
}
static int stream(const lr2021_iq_hal *h, unsigned duration,
                  lr2021_iq_sink sink, void *context, lr2021_iq_result *r, bool checked, unsigned capacity)
{
    if(!r)return LR2021_IQ_ARGUMENT;
    memset(r,0,sizeof(*r)); r->capture_error=LR2021_IQ_ARGUMENT;
    if(!valid_hal(h) || !h->fast || !h->clear_fifo || !h->read_fifo ||
       !sink || !duration || duration>(checked?60000u:250u) ||
       (capacity!=256 && capacity!=1024))return LR2021_IQ_ARGUMENT;
    r->fifo_capacity=capacity;
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
    if(!e && capacity==1024)e=h->write32(c,0xF30028,0x804400);
    if(!e && capacity==1024)e=h->write32(c,0xF30030,0x3E8);
    if(!e && capacity==1024)e=verify_fifo(h);
    if(!e)e=h->write(c,0x0114,clear,2);
    if(!e)e=h->receive(c);
    if(!e && capacity==1024)e=verify_fifo(h);
    if(!e)e=h->clear_fifo(c);
    if(!e)e=h->write(c,0x0114,clear,2);
    uint64_t start=h->now_us(c);
    uint64_t previous_poll=start;
    uint8_t pending[1280]; size_t pending_bytes=0;
    while(!e && !r->stopped_by_sink && h->now_us(c)-start<(uint64_t)duration*1000) {
        uint64_t poll_time=h->now_us(c), gap=poll_time-previous_poll;
        if(gap>r->max_service_gap_us)r->max_service_gap_us=gap;
        previous_poll=poll_time;
        uint8_t level_bytes[2]={0};
        e=h->read(c,0x011C,level_bytes,2); r->polls++;
        unsigned level=((unsigned)level_bytes[0]<<8)|level_bytes[1];
        if(level>r->max_fifo_level)r->max_fifo_level=level;
        if(!e && level>=capacity)e=LR2021_IQ_DATA;
        size_t remaining=level&~3u;
        while(!e && remaining) {
            size_t n=remaining>256?256:remaining;
            uint8_t data[256]; e=h->read_fifo(c,data,n);
            if(!e) {
                uint64_t first=r->bytes/4; r->bytes+=n;
                if(checked) {
                    memcpy(pending+pending_bytes,data,n); pending_bytes+=n;
                    if(pending_bytes>=1024) {
                        uint8_t flags[2]={0}; e=h->read(c,0x011B,flags,2);
                        r->fifo_flags|=flags[0];
                        if(!e && (flags[0]&0x30))e=LR2021_IQ_DATA;
                        if(!e) {
                            bool more=sink(context,pending,pending_bytes,r->validated_bytes/4);
                            if(more)r->validated_bytes+=pending_bytes;
                            pending_bytes=0;
                            if(!more){r->stopped_by_sink=true;break;}
                        }
                    }
                } else if(!sink(context,data,n,first)){r->stopped_by_sink=true;break;}
            }
            remaining-=n;
        }
    }
    r->elapsed_us=h->now_us(c)-start;
    if(touched) {
        uint8_t flags[2]={0}; int f=h->read(c,0x011B,flags,2);
        r->fifo_flags|=flags[0];
        /* Validate and publish the final tail before stopping the producer.
         * Do not clear flags during a session: a failure must stay visible. */
        if(checked && !e && !f && !(flags[0]&0x30) && pending_bytes) {
            if(!sink(context,pending,pending_bytes,r->validated_bytes/4))r->stopped_by_sink=true;
            else r->validated_bytes+=pending_bytes;
        }
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

int lr2021_iq_stream(const lr2021_iq_hal *h, unsigned duration,
                    lr2021_iq_sink sink, void *context, lr2021_iq_result *r)
{ return stream(h,duration,sink,context,r,false,256); }

int lr2021_iq_stream_checked(const lr2021_iq_hal *h, unsigned duration,
                            lr2021_iq_sink sink, void *context, lr2021_iq_result *r)
{ return stream(h,duration,sink,context,r,true,256); }

int lr2021_iq_stream_checked_fifo(const lr2021_iq_hal *h, unsigned duration,
                                 unsigned capacity, lr2021_iq_sink sink,
                                 void *context, lr2021_iq_result *r)
{ return stream(h,duration,sink,context,r,true,capacity); }
