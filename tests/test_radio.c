#include "lr2021_iq.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
typedef struct {
    lr2021_iq_profile profile;
    uint32_t ram[256];
    uint64_t time;
    unsigned reset_count, restored, locked, blocks, mode;
    int failure, reset_failure, restore_failure;
    bool active, fast, timeout, bad_length, corrupt_restore, patched;
    uint8_t flags, stop_flags;
    unsigned level;
} fake;
static void lock(void *c){((fake *)c)->locked++;}
static void unlock(void *c){((fake *)c)->locked--;}
static uint64_t now(void *c){fake *f=c;return f->time+=100;}
static void sleep_us(void *c,unsigned n){((fake *)c)->time+=n;}
static int snapshot(void *c,lr2021_iq_profile *p){*p=((fake *)c)->profile;return 0;}
static int read32(void *c,uint32_t a,uint32_t *v)
{
    fake *f=c;
    if(a==0x800FF8){*v=f->patched?0x600DB002:0;return 0;}
    if(a==0xF30054){*v=f->timeout?0:0x03000000;return 0;}
    if(a<0x802000 || a>=0x802400 || a%4)return -90;
    *v=f->ram[(a-0x802000)/4];return 0;
}
static int write32(void *c,uint32_t a,uint32_t v)
{
    fake *f=c;
    if(a<0x802000 || a>=0x802400 || a%4)return -91;
    f->ram[(a-0x802000)/4]=v+(f->corrupt_restore?1u:0u);return 0;
}
static int write_cmd(void *c,uint16_t op,const uint8_t *p,size_t n)
{
    fake *f=c;
    if(op==0x0290 && n==11) {
        f->mode=p[5];
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
    if(op==0x011B){p[0]=f->flags;p[1]=0;return 0;}
    return -97;
}
static int standby(void *c){fake *f=c;if(f->active)f->flags|=f->stop_flags;f->active=false;return 0;}
static int receive(void *c)
{
    fake *f=c;f->active=true;
    if(f->mode==0x92)for(unsigned i=0;i<256;i++)f->ram[i]=0x12340000+i;
    return 0;
}
static int reset(void *c){fake *f=c;f->reset_count++;f->active=false;memset(f->ram,0,sizeof(f->ram));return f->reset_failure;}
static int restore(void *c,const lr2021_iq_profile *p){fake *f=c;f->restored++;f->active=p->receiving;return f->restore_failure;}
static int fast_mode(void *c,bool on){((fake *)c)->fast=on;return 0;}
static int clear(void *c){(void)c;return 0;}
static int fifo(void *c,uint8_t *p,size_t n){fake *f=c;f->blocks++;memset(p,0x42,n);return 0;}
static bool sink(void *c,const uint8_t *p,size_t n,uint64_t first)
{
    size_t *bytes=c;
    if(p[0]!=0x42 || first!=*bytes/4)return false;
    *bytes+=n;return true;
}
static bool cancel(void *c,const uint8_t *p,size_t n,uint64_t first)
{(void)c;(void)p;(void)n;(void)first;return false;}
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
    puts("capture rollback, firmware guards, timeout and FIFO failures passed");
    return 0;
}
