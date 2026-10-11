#include "lr2021_iq.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
typedef struct {
    lr2021_iq_profile profile;
    uint32_t ram[256], fifo_base, fifo_size;
    unsigned max_read; bool bad_mapping, clobber_mapping, fail_fifo_size, offset_mapping;
    uint64_t time;
    unsigned reset_count, restored, locked, blocks, mode;
    int failure, reset_failure, restore_failure;
    bool active, fast, timeout, bad_length, corrupt_restore, patched;
    uint8_t flags, stop_flags;
    unsigned level;
    unsigned flag_reads, fail_flags_at, overflow_at, receives, routes, delivered;
} fake;
static void lock(void *c){((fake *)c)->locked++;}
static void unlock(void *c){((fake *)c)->locked--;}
static uint64_t now(void *c){fake *f=c;return f->time+=100;}
static void sleep_us(void *c,unsigned n){((fake *)c)->time+=n;}
static int snapshot(void *c,lr2021_iq_profile *p){*p=((fake *)c)->profile;return 0;}
static int read32(void *c,uint32_t a,uint32_t *v)
{
    fake *f=c;
    if(a==0xF30028){*v=f->bad_mapping?0:(f->fifo_base & (f->offset_mapping?0x7FFFFF:0xFFFFFFFF));return 0;}
    if(a==0xF30030){*v=f->fifo_size;return 0;}
    if(a==0x800FF8){*v=f->patched?0x600DB002:0;return 0;}
    if(a==0xF30054){*v=f->timeout?0:0x03000000;return 0;}
    if(a<0x802000 || a>=0x802400 || a%4)return -90;
    *v=f->ram[(a-0x802000)/4];return 0;
}
static int write32(void *c,uint32_t a,uint32_t v)
{
    fake *f=c;
    if(a==0xF30028){f->fifo_base=v;return 0;}
    if(a==0xF30030){if(f->fail_fifo_size)return -101;f->fifo_size=v;return 0;}
    if(a<0x802000 || a>=0x802400 || a%4)return -91;
    f->ram[(a-0x802000)/4]=v+(f->corrupt_restore?1u:0u);return 0;
}
static int write_cmd(void *c,uint16_t op,const uint8_t *p,size_t n)
{
    fake *f=c;
    if(op==0x0290 && n==11) {
        f->mode=p[5];
        f->routes++;
        if(p[0]!=0xFF || (p[5]!=0x92 && p[5]!=0x0A))return -92;
        if(p[5]==0x92 && (p[9]!=0xFF || p[10]))return -93;
        if(p[5]==0x0A && (p[9] || p[10]!=1))return -94;
        return f->failure;
    }
    return op==0x0114 && n==2 && p[0]==0x3F && p[1]==0x3F?0:-95;
}
static int read_cmd(void *c,uint16_t op,uint8_t *p,size_t n)
{
    fake *f=c;if(n!=2)return -96;
    if(op==0x0291){p[0]=f->bad_length?3:4;p[1]=0;return 0;}
    if(op==0x011C){p[0]=(uint8_t)(f->level>>8);p[1]=(uint8_t)f->level;return 0;}
    if(op==0x011B){
        f->flag_reads++;
        if(f->fail_flags_at && f->flag_reads>=f->fail_flags_at)return -98;
        if(f->overflow_at && f->flag_reads>=f->overflow_at)f->flags|=0x10;
        p[0]=f->flags;p[1]=0;return 0;
    }
    return -97;
}
static int standby(void *c){fake *f=c;if(f->active)f->flags|=f->stop_flags;f->active=false;return 0;}
static int receive(void *c)
{
    fake *f=c;f->active=true;f->receives++;
    if(f->clobber_mapping)f->fifo_base=0;
    if(f->mode==0x92)for(unsigned i=0;i<256;i++)f->ram[i]=0x12340000+i;
    return 0;
}
static int reset(void *c){fake *f=c;f->reset_count++;f->active=false;f->fifo_base=f->fifo_size=0;memset(f->ram,0,sizeof(f->ram));return f->reset_failure;}
static int restore(void *c,const lr2021_iq_profile *p){fake *f=c;f->restored++;f->active=p->receiving;return f->restore_failure;}
static int fast_mode(void *c,bool on){((fake *)c)->fast=on;return 0;}
static int clear(void *c){(void)c;return 0;}
static int fifo(void *c,uint8_t *p,size_t n){fake *f=c;if(n>256)return -99;if(n>f->max_read)f->max_read=(unsigned)n;f->blocks++;memset(p,0x42,n);return 0;}
static bool sink(void *c,const uint8_t *p,size_t n,uint64_t first)
{
    size_t *bytes=c;
    if(p[0]!=0x42 || first!=*bytes/4)return false;
    *bytes+=n;return true;
}
static bool cancel(void *c,const uint8_t *p,size_t n,uint64_t first)
{(void)c;(void)p;(void)n;(void)first;return false;}
static bool checked_sink(void *c,const uint8_t *p,size_t n,uint64_t first)
{
    fake *f=c;
    /* Stop/restart inside the session or publication before flags is a failure. */
    if(!f->active || f->reset_count || f->routes!=1 || f->receives!=1 ||
       !f->flag_reads || (f->flags&0x30) || first!=f->delivered/4 || p[0]!=0x42)return false;
    f->delivered+=(unsigned)n;return true;
}
static lr2021_iq_hal init(fake *f)
{
    memset(f,0,sizeof(*f));
    f->profile=(lr2021_iq_profile){true,true,true,true,1,24,7,910525000,31250};
    f->level=64;
    for(unsigned i=0;i<256;i++)f->ram[i]=i;
    return (lr2021_iq_hal){f,lock,unlock,snapshot,read32,write32,write_cmd,read_cmd,
        standby,receive,reset,restore,fast_mode,clear,fifo,now,sleep_us};
}
int main(void)
{
    fake f; lr2021_iq_hal h=init(&f); lr2021_iq_result r;
    uint32_t words[256],scratch[256];
    CHECK(!lr2021_iq_capture(&h,words,scratch,256,&r));
    CHECK(r.bytes==1024 && words[255]==0x123400FF && r.nominal_rate_hz==125000);
    CHECK(f.reset_count==1 && f.restored==1 && f.active && !f.locked);
    for(unsigned i=0;i<256;i++)CHECK(f.ram[i]==i);
    h=init(&f); f.profile.fw_minor=25;
    CHECK(lr2021_iq_capture(&h,words,scratch,256,&r)==LR2021_IQ_STATE && !f.reset_count);
    h=init(&f); f.patched=true;
    CHECK(lr2021_iq_capture(&h,words,scratch,256,&r)==LR2021_IQ_STATE && !f.reset_count);
    h=init(&f);
    CHECK(lr2021_iq_capture(&h,words,words,256,&r)==LR2021_IQ_ARGUMENT && !f.locked);
    h=init(&f); f.timeout=true;
    CHECK(lr2021_iq_capture(&h,words,scratch,256,&r)==LR2021_IQ_TIMEOUT && f.restored==1);
    h=init(&f); f.bad_length=true;
    CHECK(lr2021_iq_capture(&h,words,scratch,256,&r)==LR2021_IQ_DATA && f.restored==1);
    h=init(&f); f.corrupt_restore=true;
    CHECK(lr2021_iq_capture(&h,words,scratch,256,&r)==LR2021_IQ_DATA && r.restore_error);
    h=init(&f); f.failure=-40; f.reset_failure=-41;
    CHECK(lr2021_iq_capture(&h,words,scratch,256,&r)==-40 && r.recovery_error==-41);
    h=init(&f); size_t bytes=0;
    CHECK(!lr2021_iq_stream(&h,10,sink,&bytes,&r));
    CHECK(bytes==r.bytes && bytes>0 && !f.fast && !f.locked && f.active);
    h=init(&f); CHECK(!lr2021_iq_stream(&h,10,cancel,NULL,&r) && r.stopped_by_sink);
    h=init(&f); f.flags=0x10;
    CHECK(lr2021_iq_stream(&h,10,cancel,NULL,&r)==LR2021_IQ_DATA && f.restored==1);
    h=init(&f); f.level=256;
    CHECK(lr2021_iq_stream(&h,10,cancel,NULL,&r)==LR2021_IQ_DATA && !f.blocks);
    h=init(&f); f.restore_failure=-42;
    CHECK(lr2021_iq_stream(&h,10,cancel,NULL,&r)==-42 && !f.fast && !f.locked);
    h=init(&f); f.stop_flags=0x10;
    CHECK(!lr2021_iq_stream(&h,10,cancel,NULL,&r) && !r.fifo_flags);
    h=init(&f); f.level=0;
    CHECK(lr2021_iq_stream(&h,10,cancel,NULL,&r)==LR2021_IQ_TIMEOUT && f.restored==1);
    h=init(&f); f.profile.exclusive_lora=false;
    CHECK(lr2021_iq_stream(&h,10,cancel,NULL,&r)==LR2021_IQ_STATE && !f.reset_count);
    CHECK(lr2021_iq_stream(&h,251,cancel,NULL,&r)==LR2021_IQ_ARGUMENT);
    h=init(&f);
    CHECK(!lr2021_iq_stream_checked(&h,1000,checked_sink,&f,&r));
    CHECK(!r.stopped_by_sink && r.validated_bytes==r.bytes && r.validated_bytes==f.delivered);
    CHECK(r.validated_bytes>100000 && f.flag_reads>100 && f.reset_count==1 && f.restored==1);
    CHECK(f.routes==1 && f.receives==1 && !f.fast && !f.locked);
    h=init(&f);f.overflow_at=3;
    CHECK(lr2021_iq_stream_checked(&h,1000,checked_sink,&f,&r)==LR2021_IQ_DATA);
    CHECK(r.validated_bytes==2048 && f.delivered==2048 && r.bytes==3072 && f.reset_count==1);
    h=init(&f);f.fail_flags_at=2;
    CHECK(lr2021_iq_stream_checked(&h,1000,checked_sink,&f,&r)==-98);
    CHECK(r.validated_bytes==1024 && f.delivered==1024 && r.bytes==2048);
    h=init(&f);
    CHECK(!lr2021_iq_stream_checked(&h,1000,cancel,NULL,&r));
    CHECK(r.stopped_by_sink && !r.validated_bytes && r.bytes==1024 && f.reset_count==1);
    h=init(&f);f.level=256;
    CHECK(lr2021_iq_stream_checked(&h,1000,checked_sink,&f,&r)==LR2021_IQ_DATA);
    CHECK(!r.validated_bytes && !f.delivered && !f.blocks);
    h=init(&f);f.restore_failure=-42;
    CHECK(lr2021_iq_stream_checked(&h,10,checked_sink,&f,&r)==-42);
    CHECK(r.validated_bytes==r.bytes && r.validated_bytes>0 && !f.fast && !f.locked);
    h=init(&f);f.profile.fw_minor=25;
    CHECK(lr2021_iq_stream_checked(&h,1000,checked_sink,&f,&r)==LR2021_IQ_STATE);
    CHECK(!f.routes && !f.reset_count && !f.locked);
    CHECK(lr2021_iq_stream_checked(&h,60001,checked_sink,&f,&r)==LR2021_IQ_ARGUMENT);
    h=init(&f);f.level=768;
    CHECK(!lr2021_iq_stream_checked_fifo(&h,10,1024,checked_sink,&f,&r));
    CHECK(r.fifo_capacity==1024 && r.max_fifo_level==768 && f.max_read==256);
    CHECK(f.blocks==3*r.polls); /* Drain all observed bytes before querying again. */
    CHECK(r.validated_bytes==r.bytes && r.bytes>0 && !f.fifo_base && !f.locked);
    h=init(&f);f.level=768;f.offset_mapping=true;
    CHECK(!lr2021_iq_stream_checked_fifo(&h,10,1024,checked_sink,&f,&r));
    CHECK(r.validated_bytes>0 && f.reset_count==1 && f.restored==1);
    h=init(&f);f.level=768;
    CHECK(!lr2021_iq_stream_checked_fifo(&h,10,1024,cancel,NULL,&r));
    CHECK(r.stopped_by_sink && !r.validated_bytes && f.blocks==4 && f.reset_count==1);
    h=init(&f);f.level=1024;
    CHECK(lr2021_iq_stream_checked_fifo(&h,10,1024,checked_sink,&f,&r)==LR2021_IQ_DATA);
    CHECK(!f.blocks && f.reset_count==1);
    h=init(&f);f.bad_mapping=true;
    CHECK(lr2021_iq_stream_checked_fifo(&h,10,1024,checked_sink,&f,&r)==LR2021_IQ_DATA);
    CHECK(!f.receives && f.reset_count==1 && f.restored==1);
    h=init(&f);f.clobber_mapping=true;
    CHECK(lr2021_iq_stream_checked_fifo(&h,10,1024,checked_sink,&f,&r)==LR2021_IQ_DATA);
    CHECK(f.receives==1 && !f.blocks && f.reset_count==1);
    h=init(&f);f.fail_fifo_size=true;
    CHECK(lr2021_iq_stream_checked_fifo(&h,10,1024,checked_sink,&f,&r)==-101);
    CHECK(!f.receives && !f.blocks && f.reset_count==1 && !f.fifo_base && f.restored==1);
    h=init(&f);f.level=768;f.overflow_at=2;
    CHECK(lr2021_iq_stream_checked_fifo(&h,10,1024,checked_sink,&f,&r)==LR2021_IQ_DATA);
    CHECK(r.validated_bytes==1024 && r.bytes==2048 && f.reset_count==1);
    h=init(&f);
    CHECK(lr2021_iq_stream_checked_fifo(&h,10,512,checked_sink,&f,&r)==LR2021_IQ_ARGUMENT);
    CHECK(!f.locked && !f.routes && !f.reset_count);
    puts("capture rollback, firmware guards, timeout and FIFO failures passed");
    return 0;
}
