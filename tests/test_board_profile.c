#include "board_profile.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)

typedef struct { uint8_t bytes[64]; size_t n; unsigned calls, fail; } trace;
static int write_cmd(void *ctx, uint16_t op, const uint8_t *a, size_t n)
{
    trace *t=ctx;
    if(++t->calls==t->fail) return -7;
    t->bytes[t->n++]=op>>8; t->bytes[t->n++]=op;
    memcpy(t->bytes+t->n,a,n); t->n+=n;
    return 0;
}
static const char *edit(board_profile *p, int n, char **v)
{
    board_profile next=*p;
    const char *e=board_profile_edit(&next,n,v);
    if(!e) e=board_profile_validate(&next,37,38);
    if(!e) *p=next;
    return e;
}
int main(void)
{
    board_profile p;
    board_profile_defaults(&p);
    CHECK(!board_profile_validate(&p,37,38));
    CHECK(p.xl9535 && !p.reset_high && p.busy==6 && p.irq==-1);
    CHECK(p.sclk==2 && p.mosi==3 && p.miso==4 && p.cs==24 && p.sda==7 && p.scl==8);
    /* Legacy TCXO and RF switch SPI bytes, in order. */
    static const uint8_t legacy[]={
        0x01,0x20,7,0,0,0x80,0,
        0x01,0x12,6,0x23, 0x01,0x13,6,0,
        0x01,0x12,7,0x23, 0x01,0x13,7,0,
        0x01,0x12,8,0x23, 0x01,0x13,8,2,
        0x01,0x12,10,0x23, 0x01,0x13,10,0
    };
    trace t={0};
    CHECK(!board_profile_tcxo(&p,write_cmd,&t));
    CHECK(!board_profile_rfsw(&p,write_cmd,&t));
    CHECK(t.n==sizeof(legacy) && !memcmp(t.bytes,legacy,sizeof(legacy)) && t.calls==9);
    p.tcxo=0; p.rfsw_count=0; t=(trace){0};
    CHECK(!board_profile_tcxo(&p,write_cmd,&t) && !board_profile_rfsw(&p,write_cmd,&t) && !t.n);
    board_profile_defaults(&p); t=(trace){.fail=2};
    CHECK(board_profile_rfsw(&p,write_cmd,&t)==-7 && t.calls==2);
    for(int pin=-1;pin<=60;pin++) {
        board_profile q=p; q.busy=pin;
        if(pin<0 || pin>54 || pin==26 || pin==27 || pin==37 || pin==38 ||
           pin==2 || pin==3 || pin==4 || pin==7 || pin==8 || pin==24)
            CHECK(board_profile_validate(&q,37,38));
        else CHECK(!board_profile_validate(&q,37,38));
    }
    char *reset[]={"set","reset","gpio","5","high"};
    CHECK(!edit(&p,5,reset) && !p.xl9535 && p.reset_high);
    char *irq[]={"set","irq","gpio","9","dio","9"};
    CHECK(!edit(&p,6,irq) && p.irq==9 && p.irq_dio==9);
    char *conflict[]={"rfsw","9","0","1","0","0","0"};
    board_profile saved=p;
    CHECK(edit(&p,7,conflict) && !memcmp(&p,&saved,sizeof(p)));
    char *tcxo[]={"set","tcxo","1.8"};
    CHECK(!edit(&p,3,tcxo) && p.tcxo_voltage==2);
    char *ticks[]={"set","tcxo","ticks","4294967295"};
    CHECK(!edit(&p,4,ticks) && p.tcxo_ticks==UINT32_MAX);
    t=(trace){0}; CHECK(!board_profile_tcxo(&p,write_cmd,&t));
    const uint8_t expected[]={1,0x20,2,255,255,255,255};
    CHECK(t.n==7 && !memcmp(t.bytes,expected,7));
    ticks[3]="4294967296"; CHECK(edit(&p,4,ticks));
    char *clear[]={"rfsw","clear"}; CHECK(!edit(&p,2,clear));
    char dio[3];
    char *rf[]={"rfsw",dio,"1","0","1","0","1"};
    for(unsigned i=5;i<=10;i++) {
        if(i==9) continue;
        snprintf(dio,sizeof(dio),"%u",i);
        CHECK(!edit(&p,7,rf));
    }
    char *none[]={"set","irq","none"}; CHECK(!edit(&p,3,none));
    strcpy(dio,"9"); CHECK(!edit(&p,7,rf) && p.rfsw_count==6);
    strcpy(dio,"11"); CHECK(edit(&p,7,rf));
    CHECK(p.rfsw[0].dio==5 && p.rfsw[0].pull==2 && p.rfsw[0].modes==21);
    p.rfsw[0].pull=3; CHECK(board_profile_validate(&p,37,38)); p.rfsw[0].pull=2;
    char *remove[]={"rfsw","5","remove"}; CHECK(!edit(&p,3,remove) && p.rfsw_count==5);
    CHECK(edit(&p,3,remove));
    CHECK(!edit(&p,7,rf) && p.rfsw_count==6);
    p.rfsw[0].modes=32; CHECK(board_profile_validate(&p,37,38));
    board_profile_defaults(&p); p.irq=9; p.irq_dio=4; CHECK(board_profile_validate(&p,37,38));
    board_profile_defaults(&p); p.tcxo_voltage=8; CHECK(board_profile_validate(&p,37,38));
    board_profile_defaults(&p); p.rfsw_count=7; CHECK(board_profile_validate(&p,37,38));
    board_profile_defaults(&p); p.rfsw[0].dio=12; CHECK(board_profile_validate(&p,37,38));
    puts("board defaults match legacy command bytes; edits and pin guards passed");
    return 0;
}
