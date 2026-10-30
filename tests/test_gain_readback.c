/* SPDX-License-Identifier: MIT */
#include "lr2021_sim.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)

static const uint16_t table[13]={16,28,38,49,57,64,73,85,96,108,120,127,138};
static int (*real_read32)(void *, uint32_t, uint32_t *);
static uint32_t forced_agc;
static bool forcing;
static unsigned agc_reads;
static int read32(void *c, uint32_t a, uint32_t *v)
{
    if(a==0xF30154) {
        agc_reads++;
        if(forcing){ *v=forced_agc; return 0; }
    }
    return real_read32(c,a,v);
}
typedef struct { uint64_t pairs, gaps; uint32_t last; } counter;
static bool count(void *c, const uint8_t *p, size_t n, uint64_t first)
{
    counter *k=c;
    if(first!=k->pairs || n%4)return false;
    for(size_t i=0;i<n;i+=4) {
        uint32_t v=(uint32_t)(p[i]|p[i+1]<<8)|((uint32_t)(p[i+2]|p[i+3]<<8)<<16);
        if(k->pairs && v!=k->last+1)k->gaps++;
        k->last=v; k->pairs++;
    }
    return true;
}
static bool accept(void *c, const uint8_t *p, size_t n, uint64_t first)
{ (void)p; (void)first; *(size_t *)c+=n; return true; }
typedef struct { unsigned calls; lr2021_iq_gain_state last; uint64_t time_us; } gain_log;
static void on_gain(void *c, const lr2021_iq_gain_state *g, uint64_t time_us)
{ gain_log *l=c; l->calls++; l->last=*g; l->time_us=time_us; }
static int tone_session(lr2021_iq_dev *dev, gain_log *log, lr2021_iq_result *r)
{
    size_t bytes=0;
    memset(log,0,sizeof(*log));
    lr2021_iq_session s={.duration_ms=20,.fifo_capacity=1024,.gain_context=log,
        .gain_read_every=4,.chip_gain=on_gain};
    int e=lr2021_iq_stream_session(&dev->hal,&s,accept,&bytes,r);
    return e ? e : bytes ? 0 : -1;
}
int main(void)
{
    static lr2021_sim sim; static lr2021_iq_dev dev;
    lr2021_sim_init(&sim);
    sim.pattern=LR2021_SIM_TONE; sim.tone_hz=433930000;
    lr2021_iq_bus bus=lr2021_sim_bus(&sim);
    const lr2021_iq_board board={.tcxo=1,.tcxo_voltage=7,.tcxo_ticks=32768,.rfsw_count=1,
        .rfsw={{8,2,3}}};
    lr2021_iq_rx rx={433920000,62500,1,0,LR2021_IQ_PATH_AUTO,0,0,0};
    CHECK(!lr2021_iq_dev_open(&dev,&bus,&board,&rx));
    real_read32=dev.hal.read32; dev.hal.read32=read32;

    lr2021_iq_gain_state g;
    for(uint8_t step=1;step<=13;step++) {
        rx.gain_step=step;
        CHECK(!lr2021_iq_dev_configure(&dev,&rx));
        CHECK(!lr2021_iq_read_gain(&dev.hal,&g));
        CHECK(g.step==step && g.manual==step && g.gain_value==table[step-1]);
    }

    rx.gain_step=0;
    CHECK(!lr2021_iq_dev_configure(&dev,&rx));
    CHECK(!lr2021_iq_read_gain(&dev.hal,&g));
    CHECK(g.step==13 && !g.manual && g.gain_value==139);
    const uint16_t quiet_rssi=g.rssi_raw;

    lr2021_iq_result r; gain_log log;
    CHECK(!tone_session(&dev,&log,&r));
    CHECK(r.chip_gain_reads>0 && r.chip_gain_reads==log.calls && !r.chip_gain_changes);
    CHECK(r.chip_gain_last==13 && r.chip_gain_min==13 && r.chip_gain_max==13);
    CHECK(log.last.step==13 && !log.last.manual && log.last.gain_value==138 && log.time_us);
    CHECK(log.last.rssi_raw<quiet_rssi);
    const uint16_t weak_rssi=log.last.rssi_raw;

    sim.tone_amplitude=20000;
    CHECK(!tone_session(&dev,&log,&r));
    CHECK(r.chip_gain_last==5 && r.chip_gain_min==5 && r.chip_gain_max==5);
    CHECK(log.last.step==5 && !log.last.manual && log.last.gain_value==table[4]);
    CHECK(log.last.rssi_raw<weak_rssi);

    rx.gain_step=9;
    CHECK(!lr2021_iq_dev_configure(&dev,&rx));
    CHECK(!tone_session(&dev,&log,&r));
    CHECK(r.chip_gain_last==9 && log.last.manual==9);
    sim.tone_amplitude=4000;

    forcing=true; forced_agc=0;
    CHECK(lr2021_iq_read_gain(&dev.hal,&g)==LR2021_IQ_DATA);
    forced_agc=14u<<20;
    CHECK(lr2021_iq_read_gain(&dev.hal,&g)==LR2021_IQ_DATA);
    forced_agc=15u<<20|13u<<16;
    CHECK(lr2021_iq_read_gain(&dev.hal,&g)==LR2021_IQ_DATA);
    CHECK(!tone_session(&dev,&log,&r));
    CHECK(!r.chip_gain_reads && !log.calls);
    forcing=false;
    CHECK(lr2021_iq_read_gain(NULL,&g)==LR2021_IQ_ARGUMENT);
    CHECK(lr2021_iq_read_gain(&dev.hal,NULL)==LR2021_IQ_ARGUMENT);

    sim.pattern=LR2021_SIM_COUNTER;
    rx.gain_step=13; rx.bandwidth_hz=62500;
    CHECK(!lr2021_iq_dev_configure(&dev,&rx));
    counter k={0};
    agc_reads=0;
    lr2021_iq_session s={.duration_ms=50,.fifo_capacity=1024};
    CHECK(!lr2021_iq_stream_session(&dev.hal,&s,count,&k,&r));
    CHECK(!agc_reads && !r.chip_gain_reads && !k.gaps && k.pairs>10000);
    const uint64_t baseline=k.pairs;

    memset(&k,0,sizeof(k)); memset(&log,0,sizeof(log));
    s=(lr2021_iq_session){.duration_ms=50,.fifo_capacity=1024,.gain_context=&log,
        .gain_read_every=1,.chip_gain=on_gain};
    CHECK(!lr2021_iq_stream_session(&dev.hal,&s,count,&k,&r));
    CHECK(!k.gaps && k.pairs>10000 && !(r.fifo_flags&0x30));
    CHECK(r.chip_gain_reads==agc_reads && r.chip_gain_reads==log.calls && r.chip_gain_reads>100);
    CHECK(k.pairs*10>=baseline*9);

    memset(&k,0,sizeof(k)); agc_reads=0;
    s=(lr2021_iq_session){.duration_ms=50,.fifo_capacity=1024,.gain_read_every=16};
    CHECK(!lr2021_iq_stream_session(&dev.hal,&s,count,&k,&r));
    CHECK(!k.gaps && r.chip_gain_reads==agc_reads && r.chip_gain_reads>0);
    puts("gain readback: manual steps, auto, range checks and session sampling passed");
    return 0;
}
