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
static int read_gain(const lr2021_iq_hal *h, lr2021_iq_gain_state *g)
{
    uint32_t agc=0,value=0,rssi=0;
    int e=h->read32(h->context,0xF30154,&agc);
    if(!e)e=h->read32(h->context,0xF3012C,&value);
    if(!e)e=h->read32(h->context,0xF301A8,&rssi);
    if(e)return e;
    g->step=(uint8_t)(agc>>20&15); g->manual=(uint8_t)(agc>>16&15);
    g->gain_value=(uint16_t)(value>>18&0x1FF); g->rssi_raw=(uint16_t)(rssi>>18&0x1FF);
    return g->step<1 || g->step>13 ? LR2021_IQ_DATA : 0;
}
int lr2021_iq_read_gain(const lr2021_iq_hal *h, lr2021_iq_gain_state *g)
{
    if(g)memset(g,0,sizeof(*g));
    if(!h || !h->lock || !h->unlock || !h->snapshot || !h->read32 || !g)return LR2021_IQ_ARGUMENT;
    h->lock(h->context);
    lr2021_iq_profile p={0};
    int e=h->snapshot(h->context,&p);
    if(!e && (!p.lr2021 || p.fw_major!=1 || p.fw_minor!=24))e=LR2021_IQ_STATE;
    if(!e)e=read_gain(h,g);
    h->unlock(h->context);
    return e;
}
static void note_gain(lr2021_iq_result *r, lr2021_iq_session *s, const lr2021_iq_gain_state *g,
                      uint64_t time_us)
{
    if(!r->chip_gain_reads || g->step<r->chip_gain_min)r->chip_gain_min=g->step;
    if(g->step>r->chip_gain_max)r->chip_gain_max=g->step;
    if(r->chip_gain_reads && g->step!=r->chip_gain_last)r->chip_gain_changes++;
    r->chip_gain_last=g->step; r->chip_gain_reads++;
    if(s->chip_gain)s->chip_gain(s->gain_context,g,time_us);
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
                  lr2021_iq_sink sink, void *context, lr2021_iq_result *r, bool checked,
                  unsigned capacity, lr2021_iq_session *session)
{
    if(!r)return LR2021_IQ_ARGUMENT;
    memset(r,0,sizeof(*r)); r->capture_error=LR2021_IQ_ARGUMENT;
    if(!valid_hal(h) || !h->fast || !h->clear_fifo || !h->read_fifo ||
       !sink || (!duration && !session) || duration>(checked?60000u:250u) ||
       (capacity!=256 && capacity!=1024))return LR2021_IQ_ARGUMENT;
    /* Session tuning: FIFO read size and flag-check interval, staged in the
     * caller's buffer when given. Defaults are 256-byte reads and a check per
     * 1 KiB, as in the other APIs. */
    uint8_t local[1280];
    uint8_t *pending=local;
    size_t cap=sizeof(local), chunk=256, check=1024;
    if(session) {
        if(session->max_read)chunk=session->max_read;
        if(session->check_bytes)check=session->check_bytes;
        if(session->buffer){ pending=session->buffer; cap=session->buffer_size; }
        if(chunk%4 || chunk>1020 || check<chunk || check%4 || check+chunk>cap)return LR2021_IQ_ARGUMENT;
    }
    const unsigned level_every=checked && session && session->level_every>1 ? session->level_every : 1;
    r->fifo_capacity=capacity;
    void *c=h->context;
    h->lock(c);
    lr2021_iq_profile p={0}; int e=preflight(h,&p);
    if(!e && !session && (p.bandwidth_hz!=31250 || p.route))e=LR2021_IQ_STATE;
    if(!e && session && !p.route && p.bandwidth_hz!=31250 && p.bandwidth_hz!=62500 &&
       p.bandwidth_hz!=125000 && p.bandwidth_hz!=250000)e=LR2021_IQ_STATE;
    if(!e && session && (p.route>3 || (p.route && !p.route_rate_hz)))e=LR2021_IQ_STATE;
    const bool raw=session && p.route;
    const unsigned pair_bytes=raw?2u:4u;
    bool touched=!e, fast=false;
    if(!e){e=h->fast(c,true);fast=!e;}
    if(!e)e=h->standby(c);
    /* 0x0290 route: byte 0 input code, bytes 1..4 output rate, byte 5 0x08|source
     * for FIFO output, last byte FIFO enable. Source 2 uses its native rate. */
    static const uint8_t sources[4]={2,0,1,6};
    uint8_t args[]={0xFF,0,0,0,0,0x0A,0,0,0,0,1};
    if(raw) {
        uint32_t q=p.route_rate_hz;
        args[0]=p.input_code; args[1]=(uint8_t)(q>>24); args[2]=(uint8_t)(q>>16);
        args[3]=(uint8_t)(q>>8); args[4]=(uint8_t)q; args[5]=(uint8_t)(0x08|sources[p.route]);
    }
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
    size_t pending_bytes=0;
    /* Level prediction state: the last FIFO level read, when, and bytes read since. */
    const uint64_t rate_bytes=(uint64_t)(raw?p.route_rate_hz:4*p.bandwidth_hz)*pair_bytes;
    uint64_t sample_us=0; size_t sample_level=0, drained=0; unsigned pass=0;
    unsigned gain_pass=0;
    r->start_us=start;
    if(session)session->start_us=start;
    while(!e && !r->stopped_by_sink && (!duration || h->now_us(c)-start<(uint64_t)duration*1000)) {
        uint64_t poll_time=h->now_us(c), gap=poll_time-previous_poll;
        if(gap>r->max_service_gap_us)r->max_service_gap_us=gap;
        previous_poll=poll_time;
        uint8_t step,boost;
        if(session && session->gain && session->gain(session->gain_context,&step,&boost)) {
            /* Live gain change: the FIFO keeps running, no restart. */
            const uint8_t path[2]={p.rx_path,boost};
            if(step>13 || boost>7)e=LR2021_IQ_ARGUMENT;
            if(!e)e=h->write(c,0x021A,&step,1);
            /* SetRxPath is only sent when the boost changes: on ROM 1.24 the gain
             * step is taken in RX, while SetRxPath there ended the session with a
             * command error (measured on the LilyGO board). */
            if(!e && boost!=p.rx_boost)e=h->write(c,0x0201,path,2);
            if(e)break;
            p.gain_step=step; p.rx_boost=boost; r->gain_changes++;
        }
        if(session && session->gain_read_every && !(gain_pass++%session->gain_read_every)) {
            lr2021_iq_gain_state g;
            int ge=read_gain(h,&g);
            if(!ge)note_gain(r,session,&g,h->now_us(c));
            else if(ge!=LR2021_IQ_DATA){e=ge;break;}
        }
        size_t remaining;
        if(level_every>1 && pass%level_every) {
            /* Predicted pass: read 90% of what the nominal rate delivered since
             * the last level read. An over-read sets the sticky underflow flag,
             * which rejects the staged block at its check before publication. */
            uint64_t arrived=(h->now_us(c)-sample_us)*rate_bytes*9/10/1000000u;
            uint64_t est=sample_level+arrived>drained?sample_level+arrived-drained:0;
            remaining=(size_t)(est<capacity?est:capacity)&~3u;
            /* Only a pass that reads counts towards the next level read. */
            if(remaining>=64)pass++;
            else remaining=0;
        } else {
            pass++;
            uint8_t level_bytes[2]={0};
            e=h->read(c,0x011C,level_bytes,2); r->polls++;
            unsigned level=((unsigned)level_bytes[0]<<8)|level_bytes[1];
            if(level_every>1){ sample_us=h->now_us(c); sample_level=level; drained=0; }
            if(level>r->max_fifo_level)r->max_fifo_level=level;
            if(!e && level>=capacity)e=LR2021_IQ_DATA;
            remaining=level&~3u;
        }
        while(!e && remaining) {
            size_t n=remaining>chunk?chunk:remaining;
            uint8_t data[256];
            /* Checked streams read straight into the staging buffer. */
            e=h->read_fifo(c,checked?pending+pending_bytes:data,n);
            if(!e) {
                uint64_t first=r->bytes/4; r->bytes+=n;
                if(level_every>1)drained+=n;
                if(checked) {
                    pending_bytes+=n;
                    if(pending_bytes>=check) {
                        uint8_t flags[2]={0}; e=h->read(c,0x011B,flags,2);
                        r->fifo_flags|=flags[0];
                        if(!e && (flags[0]&0x30))e=LR2021_IQ_DATA;
                        if(!e) {
                            bool more=sink(context,pending,pending_bytes,r->validated_bytes/pair_bytes);
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
            if(!sink(context,pending,pending_bytes,r->validated_bytes/pair_bytes))r->stopped_by_sink=true;
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
    r->capture_error=e;
    r->nominal_rate_hz=raw?p.route_rate_hz:session?4*p.bandwidth_hz:125000;
    h->unlock(c); return outcome(r);
}

int lr2021_iq_stream(const lr2021_iq_hal *h, unsigned duration,
                    lr2021_iq_sink sink, void *context, lr2021_iq_result *r)
{ return stream(h,duration,sink,context,r,false,256,NULL); }

int lr2021_iq_stream_checked(const lr2021_iq_hal *h, unsigned duration,
                            lr2021_iq_sink sink, void *context, lr2021_iq_result *r)
{ return stream(h,duration,sink,context,r,true,256,NULL); }

int lr2021_iq_stream_checked_fifo(const lr2021_iq_hal *h, unsigned duration,
                                 unsigned capacity, lr2021_iq_sink sink,
                                 void *context, lr2021_iq_result *r)
{ return stream(h,duration,sink,context,r,true,capacity,NULL); }

int lr2021_iq_stream_session(const lr2021_iq_hal *h, lr2021_iq_session *s,
                            lr2021_iq_sink sink, void *context, lr2021_iq_result *r)
{
    if(!s) {
        if(r){memset(r,0,sizeof(*r)); r->capture_error=LR2021_IQ_ARGUMENT;}
        return LR2021_IQ_ARGUMENT;
    }
    return stream(h,s->duration_ms,sink,context,r,true,s->fifo_capacity,s);
}
