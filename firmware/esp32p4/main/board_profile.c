#include "board_profile.h"
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#else
#define CONFIG_LRIQ_LILYGO 1
#define CONFIG_LRIQ_RESET 5
#define CONFIG_LRIQ_BUSY 6
#define CONFIG_LRIQ_IRQ -1
#define CONFIG_LRIQ_IRQ_DIO 9
#define CONFIG_LRIQ_SCLK 2
#define CONFIG_LRIQ_MOSI 3
#define CONFIG_LRIQ_MISO 4
#define CONFIG_LRIQ_CS 24
#define CONFIG_LRIQ_SDA 7
#define CONFIG_LRIQ_SCL 8
#define CONFIG_LRIQ_TCXO 1
#define CONFIG_LRIQ_TCXO_VOLTAGE 7
#define CONFIG_LRIQ_TCXO_TICKS 32768
#define CONFIG_LRIQ_RFSW 1
#endif

void board_profile_defaults(board_profile *p)
{
    memset(p,0,sizeof(*p));
#ifdef CONFIG_LRIQ_LILYGO
    p->xl9535=1;
#endif
#ifdef CONFIG_LRIQ_RESET_HIGH
    p->reset_high=!p->xl9535;
#endif
    p->reset=CONFIG_LRIQ_RESET; p->busy=CONFIG_LRIQ_BUSY;
    p->irq=CONFIG_LRIQ_IRQ; p->irq_dio=p->irq<0?-1:CONFIG_LRIQ_IRQ_DIO;
    p->sclk=CONFIG_LRIQ_SCLK; p->mosi=CONFIG_LRIQ_MOSI;
    p->miso=CONFIG_LRIQ_MISO; p->cs=CONFIG_LRIQ_CS;
    p->sda=CONFIG_LRIQ_SDA; p->scl=CONFIG_LRIQ_SCL;
#ifdef CONFIG_LRIQ_TCXO
    p->tcxo=1;
#endif
    p->tcxo_voltage=CONFIG_LRIQ_TCXO_VOLTAGE; p->tcxo_ticks=CONFIG_LRIQ_TCXO_TICKS;
#ifdef CONFIG_LRIQ_RFSW
    p->rfsw_count=4;
    p->rfsw[0]=(board_rfsw){6,0,3}; p->rfsw[1]=(board_rfsw){7,0,3};
    p->rfsw[2]=(board_rfsw){8,2,3}; p->rfsw[3]=(board_rfsw){10,0,3};
#endif
}
static const char *gpio_reason(int pin, int tx, int rx)
{
    /* P4 memory and HS USB pads have no GPIO numbers. */
    if(pin<0 || pin>54) return "GPIO must be 0..54; memory pads are unavailable";
    if(pin==26 || pin==27) return "GPIO is reserved for USB";
    if(pin==tx || pin==rx) return "GPIO is reserved for the console UART";
    return NULL;
}
const char *board_profile_validate(const board_profile *p, int tx, int rx)
{
    if(p->xl9535>1 || p->reset_high>1 || p->tcxo>1) return "invalid board flags";
    if(p->xl9535 && p->reset_high) return "XL9535 reset must be active low";
    const int stored[]={p->reset,p->sda,p->scl};
    for(unsigned i=0;i<sizeof(stored)/sizeof(stored[0]);i++) {
        const char *reason=gpio_reason(stored[i],tx,rx);
        if(reason) return reason;
    }
    int pins[9]={p->busy,p->sclk,p->mosi,p->miso,p->cs};
    unsigned n=5;
    if(p->xl9535) { pins[n++]=p->sda; pins[n++]=p->scl; }
    else pins[n++]=p->reset;
    if(p->irq!=-1) {
        if(p->irq_dio<5 || p->irq_dio>11) return "IRQ DIO must be 5..11";
        pins[n++]=p->irq;
    } else if(p->irq_dio!=-1) return "IRQ none requires no DIO";
    for(unsigned i=0;i<n;i++) {
        const char *reason=gpio_reason(pins[i],tx,rx);
        if(reason) return reason;
        for(unsigned j=0;j<i;j++) if(pins[i]==pins[j]) return "GPIO is used by another board signal";
    }
    if(p->tcxo_voltage>7) return "TCXO voltage code must be 0..7";
    if(p->rfsw_count>BOARD_RFSW_MAX) return "RF switch table allows up to 6 entries";
    for(unsigned i=0;i<p->rfsw_count;i++) {
        const board_rfsw *r=&p->rfsw[i];
        if(r->dio<5 || r->dio>11) return "RF switch DIO must be 5..11";
        if(r->modes>31) return "RF switch mode bits must be 0..31";
        if(r->pull>3 || (r->dio==5 && r->pull!=2)) return "invalid sleep pull; DIO5 requires pull 2";
        if(p->irq>=0 && r->dio==p->irq_dio) return "DIO is assigned to IRQ";
        for(unsigned j=0;j<i;j++) if(r->dio==p->rfsw[j].dio) return "duplicate RF switch DIO";
    }
    return NULL;
}
static bool number(const char *s, uint32_t max, uint32_t *out)
{
    char *end;
    if(!*s || *s=='-' || *s=='+') return false;
    unsigned long long v=strtoull(s,&end,10);
    if(*end || v>max) return false;
    *out=(uint32_t)v; return true;
}
const char *board_profile_edit(board_profile *p, int argc, char **v)
{
    uint32_t a,b;
    if(argc==2 && !strcmp(v[0],"rfsw") && !strcmp(v[1],"clear")) {
        p->rfsw_count=0; memset(p->rfsw,0,sizeof(p->rfsw)); return NULL;
    }
    if(argc>=2 && !strcmp(v[0],"rfsw")) {
        if(!number(v[1],11,&a) || a<5) return "RF switch DIO must be 5..11";
        unsigned i=0;
        while(i<p->rfsw_count && p->rfsw[i].dio!=a) i++;
        if(argc==3 && !strcmp(v[2],"remove")) {
            if(i==p->rfsw_count) return "RF switch DIO is not in the table";
            memmove(&p->rfsw[i],&p->rfsw[i+1],(p->rfsw_count-i-1)*sizeof(p->rfsw[0]));
            memset(&p->rfsw[--p->rfsw_count],0,sizeof(p->rfsw[0])); return NULL;
        }
        if(argc!=7 && argc!=8) return "usage: board rfsw <dio> <standby> <rx_lf> <tx_lf> <rx_hf> <tx_hf> [pull]";
        board_rfsw r={(uint8_t)a,0,a==5?2:3};
        for(unsigned k=0;k<5;k++) {
            if(!number(v[k+2],1,&b)) return "RF switch levels must be 0 or 1";
            r.modes|=b<<k;
        }
        if(argc==8) {
            if(!number(v[7],3,&b)) return "sleep pull must be 0..3";
            r.pull=b;
        }
        if(i==BOARD_RFSW_MAX) return "RF switch table is full";
        p->rfsw[i]=r;
        if(i==p->rfsw_count) p->rfsw_count++;
        return NULL;
    }
    if(argc<3 || strcmp(v[0],"set")) return "usage: board [set|rfsw|save|default]";
    if(!strcmp(v[1],"reset")) {
        if(argc==3 && !strcmp(v[2],"xl9535")) {
            p->xl9535=1; p->reset_high=0; return NULL;
        }
        if(argc!=5 || strcmp(v[2],"gpio") || !number(v[3],54,&a) ||
           (strcmp(v[4],"low") && strcmp(v[4],"high"))) return "usage: board set reset gpio <pin> <low|high>";
        p->xl9535=0; p->reset=a; p->reset_high=!strcmp(v[4],"high"); return NULL;
    }
    if(!strcmp(v[1],"irq")) {
        if(argc==3 && !strcmp(v[2],"none")) { p->irq=p->irq_dio=-1; return NULL; }
        if(argc!=6 || strcmp(v[2],"gpio") || strcmp(v[4],"dio") ||
           !number(v[3],54,&a) || !number(v[5],11,&b) || b<5)
            return "usage: board set irq none OR board set irq gpio <pin> dio <5..11>";
        p->irq=a; p->irq_dio=b; return NULL;
    }
    if(!strcmp(v[1],"tcxo")) {
        static const char *volts[]={"1.6","1.7","1.8","2.2","2.4","2.7","3.0","3.3"};
        if(argc==3 && !strcmp(v[2],"off")) { p->tcxo=0; return NULL; }
        if(argc==4 && !strcmp(v[2],"ticks") && number(v[3],UINT32_MAX,&a)) {
            p->tcxo_ticks=a; return NULL;
        }
        if(argc==3) for(unsigned i=0;i<8;i++) if(!strcmp(v[2],volts[i])) {
            p->tcxo=1; p->tcxo_voltage=i; return NULL;
        }
        return "TCXO: off, 1.6, 1.7, 1.8, 2.2, 2.4, 2.7, 3.0, 3.3 or ticks <0..4294967295>";
    }
    if(argc!=3 || !number(v[2],54,&a)) return "GPIO must be 0..54";
    int8_t *pin=NULL;
    if(!strcmp(v[1],"busy")) pin=&p->busy;
    if(!strcmp(v[1],"sclk")) pin=&p->sclk;
    if(!strcmp(v[1],"mosi")) pin=&p->mosi;
    if(!strcmp(v[1],"miso")) pin=&p->miso;
    if(!strcmp(v[1],"cs")) pin=&p->cs;
    if(!strcmp(v[1],"sda")) pin=&p->sda;
    if(!strcmp(v[1],"scl")) pin=&p->scl;
    if(!pin) return "unknown board setting";
    *pin=a; return NULL;
}
int board_profile_tcxo(const board_profile *p, board_write write, void *ctx)
{
    if(!p->tcxo) return 0;
    uint32_t t=p->tcxo_ticks;
    const uint8_t a[]={p->tcxo_voltage,t>>24,t>>16,t>>8,t};
    return write(ctx,0x0120,a,sizeof(a));
}
int board_profile_rfsw(const board_profile *p, board_write write, void *ctx)
{
    int e=0;
    for(unsigned i=0;!e && i<p->rfsw_count;i++) {
        uint8_t a[]={p->rfsw[i].dio,0x20|p->rfsw[i].pull};
        e=write(ctx,0x0112,a,2);
        a[1]=p->rfsw[i].modes;
        if(!e) e=write(ctx,0x0113,a,2);
    }
    return e;
}
