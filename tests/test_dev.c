/* SPDX-License-Identifier: GPL-3.0-only */
#include "lr2021_iq_tx.h"
#include "lr2021_sim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)

typedef struct { uint64_t pairs, next, gaps; uint32_t last; size_t blocks, stop_after; int requests; } counter;
static bool count(void *c, const uint8_t *p, size_t n, uint64_t first)
{
    counter *k=c;
    if(first!=k->pairs || n%4 || (k->stop_after && k->blocks+1>=k->stop_after))return false;
    for(size_t i=0;i<n;i+=4) {
        uint32_t v=(uint32_t)(p[i]|p[i+1]<<8)|((uint32_t)(p[i+2]|p[i+3]<<8)<<16);
        if(k->pairs && v!=k->last+1)k->gaps++;
        k->last=v; k->pairs++;
    }
    k->blocks++;
    return true;
}
static bool gain(void *c, uint8_t *step, uint8_t *boost)
{
    counter *k=c;
    if(k->blocks!=3 || k->requests)return false;
    k->requests++; *step=12; *boost=0; return true;
}
int main(void)
{
    static lr2021_sim sim; static lr2021_iq_dev dev;
    lr2021_sim_init(&sim);
    lr2021_iq_bus bus=lr2021_sim_bus(&sim);
    const lr2021_iq_board board={.tcxo=1,.tcxo_voltage=7,.tcxo_ticks=32768,.rfsw_count=1,
        .rfsw={{8,2,3}}};
    lr2021_iq_rx rx={154785000,31250,13,0,LR2021_IQ_PATH_AUTO};
    CHECK(lr2021_iq_rate(31250)==125000 && lr2021_iq_rate(62500)==250000 && !lr2021_iq_rate(125000));
    CHECK(lr2021_iq_bandwidth(250000)==62500 && lr2021_iq_path(2400000000u,2)==1 && !lr2021_iq_path(1499999999u,2));
    CHECK(!lr2021_iq_dev_open(&dev,&bus,&board,&rx));
    CHECK(dev.profile.configured && dev.profile.fw_minor==24 && sim.lora_bw==2 && sim.step==13);
    CHECK(sim.freq==154785000 && sim.path==0 && !dev.errors);

    /* Tuning limits: 100 MHz configures with IMG_CAL reported, 99 MHz and
     * 2741 MHz are refused before touching the radio. */
    rx.frequency_hz=100000000;
    CHECK(!lr2021_iq_dev_configure(&dev,&rx) && dev.errors==LR2021_IQ_ERR_IMG_CAL);
    rx.frequency_hz=99999999; CHECK(lr2021_iq_dev_configure(&dev,&rx)==LR2021_IQ_ARGUMENT);
    rx.frequency_hz=2741000000u; CHECK(lr2021_iq_dev_configure(&dev,&rx)==LR2021_IQ_ARGUMENT);
    rx.frequency_hz=2740000000u;
    CHECK(!lr2021_iq_dev_configure(&dev,&rx) && sim.path==1 && !dev.errors);
    rx.frequency_hz=433920000; rx.bandwidth_hz=62500;
    CHECK(!lr2021_iq_dev_configure(&dev,&rx) && sim.lora_bw==3 && sim.path==0);
    rx.bandwidth_hz=125000; CHECK(lr2021_iq_dev_configure(&dev,&rx)==LR2021_IQ_ARGUMENT);

    /* 250 ksample/s session: continuous counter, live gain change, restore keeps it. */
    lr2021_iq_result r; counter k={0}; k.stop_after=400;
    lr2021_iq_session s={0,1024,gain,&k,0};
    CHECK(!lr2021_iq_stream_session(&dev.hal,&s,count,&k,&r));
    CHECK(r.stopped_by_sink && r.nominal_rate_hz==250000 && r.gain_changes==1 && !k.gaps);
    CHECK(k.pairs==r.validated_bytes/4 && k.pairs>=399*256 && s.start_us && s.start_us==r.start_us);
    CHECK(sim.step==12 && !sim.boost && dev.rx.gain_step==12 && !dev.rx.boost);
    CHECK(r.max_fifo_level<1024 && !(r.fifo_flags&0x30) && dev.profile.configured);

    /* Bounded session and an injected overflow. */
    rx.bandwidth_hz=31250; CHECK(!lr2021_iq_dev_configure(&dev,&rx));
    memset(&k,0,sizeof(k)); s=(lr2021_iq_session){50,1024,NULL,NULL,0};
    int e=lr2021_iq_stream_session(&dev.hal,&s,count,&k,&r);
    if(e)fprintf(stderr,"e=%d cap=%d rec=%d flags=%x bytes=%zu lvl=%u\n",e,r.capture_error,r.recovery_error,r.fifo_flags,r.bytes,r.max_fifo_level);
    CHECK(!e);
    CHECK(!r.stopped_by_sink && !k.gaps && r.nominal_rate_hz==125000);
    CHECK(k.pairs>5000 && k.pairs<7000);
    memset(&k,0,sizeof(k)); sim.overflow_after=sim.rx_pairs+20000;
    s=(lr2021_iq_session){0,1024,NULL,NULL,0};
    CHECK(lr2021_iq_stream_session(&dev.hal,&s,count,&k,&r)==LR2021_IQ_DATA);
    CHECK((r.fifo_flags&0x10) && !k.gaps && r.validated_bytes/4==k.pairs && dev.profile.configured);
    sim.overflow_after=0;
    CHECK(lr2021_iq_stream_session(&dev.hal,NULL,count,&k,&r)==LR2021_IQ_ARGUMENT);
    s.fifo_capacity=512; CHECK(lr2021_iq_stream_session(&dev.hal,&s,count,&k,&r)==LR2021_IQ_ARGUMENT);

    /* TX contract checks. */
    static int8_t iq[2*32767];
    for(unsigned i=0;i<32767;i++){ iq[2*i]=(int8_t)(i%97-48); iq[2*i+1]=(int8_t)(i%31-15); }
    lr2021_iq_tx t={iq,1000,1,9600,433920000,LR2021_IQ_TX_DEFAULT_POWER,0,0,0};
    CHECK(!lr2021_iq_tx_check(&t));
    t.rate_hz=10000; CHECK(lr2021_iq_tx_check(&t)); t.rate_hz=9600;
    t.pairs=32767; t.rate_hz=125000; CHECK(!lr2021_iq_tx_check(&t));
    t.pairs=32768; CHECK(lr2021_iq_tx_check(&t));
    t.pairs=4321; t.rate_hz=4800; CHECK(lr2021_iq_tx_check(&t)); /* > 900 ms */
    t.pairs=4320; CHECK(!lr2021_iq_tx_check(&t));
    t.pairs=2000; t.repeats=17; CHECK(lr2021_iq_tx_check(&t)); t.repeats=1;
    t.frequency_hz=1000000000u; CHECK(lr2021_iq_tx_check(&t)); t.frequency_hz=915000000;
    t.power_half_db=21; CHECK(lr2021_iq_tx_check(&t)); t.power_half_db=-18;
    /* Unchecked: only hardware limits, the rest reported as untested. */
    t.flags=LR2021_IQ_TX_UNCHECKED; t.rate_hz=10000; t.frequency_hz=2440000000u; t.pa=LR2021_IQ_PA_AUTO;
    t.power_half_db=24; t.repeats=16; t.pairs=2047;
    CHECK(!lr2021_iq_tx_check(&t) && lr2021_iq_tx_pa(&t)==1);
    CHECK(lr2021_iq_tx_untested(&t)==(LR2021_IQ_TX_UNTESTED_RATE|LR2021_IQ_TX_UNTESTED_FREQUENCY|
        LR2021_IQ_TX_UNTESTED_HF_PA|LR2021_IQ_TX_UNTESTED_POWER|LR2021_IQ_TX_UNTESTED_DURATION));
    t.power_half_db=25; CHECK(lr2021_iq_tx_check(&t)); t.power_half_db=0;
    t.pairs=2048; CHECK(lr2021_iq_tx_check(&t)); t.pairs=2047;       /* ROM 16-bit count */
    t.frequency_hz=2741000000u; CHECK(lr2021_iq_tx_check(&t));
    t=(lr2021_iq_tx){iq,32767,1,4800,433920000,-18,0,0,LR2021_IQ_TX_UNCHECKED};
    CHECK(!lr2021_iq_tx_check(&t) && lr2021_iq_tx_untested(&t)==LR2021_IQ_TX_UNTESTED_DURATION);
    t=(lr2021_iq_tx){iq,1000,1,9600,915000000,-18,0,0,0};
    iq[10]=40; iq[11]=40; CHECK(lr2021_iq_tx_check(&t)); iq[10]=39; iq[11]=39;
    CHECK(!lr2021_iq_tx_check(&t));

    /* Repeated 125 ksample/s burst: the simulator records every sample the
     * TX path consumed; it must equal the buffer repeated exactly. */
    static uint8_t captured[65536];
    sim.tx_capture=captured; sim.tx_capacity=sizeof(captured); sim.tx_captured=0;
    lr2021_iq_tx_result tr;
    t=(lr2021_iq_tx){iq,3001,10,125000,915000000,-18,0,0,0};
    unsigned resets=sim.resets;
    CHECK(!lr2021_iq_transmit(&dev.hal,&board,&t,&tr));
    CHECK(sim.tx_captured==60020 && tr.queued_bytes==60020 && !(tr.tx_flags&0x30));
    for(size_t i=0;i<60020;i++)CHECK(captured[i]==(uint8_t)iq[i%6002]);
    CHECK((tr.irq&(1u<<19)) && tr.final_level==0 && tr.min_level<1024 && tr.refills>0);
    CHECK(sim.resets==resets+1 && dev.profile.configured && sim.packet_type==0 && sim.lora_bw==2);
    CHECK(tr.elapsed_us>=240000 && tr.elapsed_us<300000);

    /* TX FIFO frames are limited to 62 data bytes. */
    CHECK(dev.hal.write_fifo(dev.hal.context,(const uint8_t *)iq,63)==LR2021_IQ_ARGUMENT);
    /* SIMO flag: same burst with the DC-DC regulator requested. */
    sim.tx_captured=0;
    t=(lr2021_iq_tx){iq,960,1,9600,433920000,-18,0,0,LR2021_IQ_TX_SIMO};
    CHECK(!lr2021_iq_tx_check(&t) && !lr2021_iq_transmit(&dev.hal,NULL,&t,&tr) && sim.tx_captured==1920);
    /* Timed burst waits for the requested device time before SetTx. */
    sim.tx_captured=0;
    uint64_t when=lr2021_sim_now_us(&sim)+200000;
    t=(lr2021_iq_tx){iq,960,1,9600,433920000,-18,when,0,0};
    CHECK(!lr2021_iq_transmit(&dev.hal,NULL,&t,&tr));
    CHECK(tr.started_us>=when && tr.started_us<when+2000 && sim.tx_captured==1920);
    t.start_us=lr2021_sim_now_us(&sim)+20000000u;
    CHECK(lr2021_iq_transmit(&dev.hal,NULL,&t,&tr)==LR2021_IQ_ARGUMENT && !tr.recovery_error);

    /* RX after TX resumes the saved profile. */
    memset(&k,0,sizeof(k)); s=(lr2021_iq_session){20,1024,NULL,NULL,0};
    CHECK(!lr2021_iq_stream_session(&dev.hal,&s,count,&k,&r) && !k.gaps && k.pairs>2000);
    puts("device, session, live gain, TX burst and recovery tests passed");
    return 0;
}
