// SPDX-License-Identifier: GPL-3.0-only
// The lr2021 SoapySDR module against the in-process simulator.
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#define CHECK(x) do { if(!(x)) { std::fprintf(stderr,"line %d: %s\n",__LINE__,#x); return 1; } } while(0)

static long long now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
int main()
{
    auto found=SoapySDR::Device::enumerate("driver=lr2021,sim=counter");
    CHECK(found.size()==1 && found[0]["driver"]=="lr2021");
    SoapySDR::Device *d=SoapySDR::Device::make("driver=lr2021,sim=counter");
    CHECK(d && d->getDriverKey()=="lr2021" && d->getHardwareInfo()["backend"]=="sim");
    CHECK(d->getHardwareInfo()["rom"]=="1.24");
    auto fr=d->getFrequencyRange(SOAPY_SDR_RX,0);
    CHECK(fr.size()==1 && fr[0].minimum()==100e6 && fr[0].maximum()==2740e6);
    auto tfr=d->getFrequencyRange(SOAPY_SDR_TX,0);
    CHECK(tfr[0].minimum()==100e6 && tfr[0].maximum()==2740e6);
    d->writeSetting("tx_checked","true");
    tfr=d->getFrequencyRange(SOAPY_SDR_TX,0);
    CHECK(tfr[0].minimum()==150e6 && tfr[0].maximum()==960e6);
    d->writeSetting("tx_checked","false");
    CHECK(d->listSampleRates(SOAPY_SDR_RX,0).size()==4 && d->listSampleRates(SOAPY_SDR_TX,0).size()==6);
    bool threw=false;
    try { d->setSampleRate(SOAPY_SDR_RX,0,300000); } catch(const std::exception &) { threw=true; }
    CHECK(threw);
    threw=false;
    try { d->setFrequency(SOAPY_SDR_RX,0,99e6); } catch(const std::exception &) { threw=true; }
    CHECK(threw);
    d->setFrequency(SOAPY_SDR_RX,0,433.92e6);
    d->setSampleRate(SOAPY_SDR_RX,0,250000);
    CHECK(d->getSampleRate(SOAPY_SDR_RX,0)==250000 && d->getFrequency(SOAPY_SDR_RX,0)==433.92e6);
    d->setGain(SOAPY_SDR_RX,0,17); // step 13, boost 4
    CHECK(d->getGain(SOAPY_SDR_RX,0,"STEP")==13 && d->getGain(SOAPY_SDR_RX,0,"BOOST")==4);
    d->setGainMode(SOAPY_SDR_RX,0,true);
    CHECK(d->getGainMode(SOAPY_SDR_RX,0));
    d->setGainMode(SOAPY_SDR_RX,0,false);
    CHECK(!d->getGainMode(SOAPY_SDR_RX,0) && d->getGain(SOAPY_SDR_RX,0,"STEP")==13);
    CHECK(d->hasHardwareTime(""));
    d->setHardwareTime(5000000000LL,"");
    long long t=d->getHardwareTime("");
    CHECK(t>=5000000000LL && t<5100000000LL);

    // RX, CS16: counter continuity, timestamps that advance with the sample count.
    SoapySDR::Stream *rx=d->setupStream(SOAPY_SDR_RX,SOAPY_SDR_CS16);
    CHECK(d->activateStream(rx)==0);
    std::vector<int16_t> buf(2*4096);
    void *bufs[]={buf.data()};
    uint64_t pairs=0; uint32_t last=0; bool have=false; unsigned gaps=0, overflows=0;
    long long first_t=0, expect_t=0; double worst_dt=0;
    long long end=now_ns()+1000000000LL;
    while(now_ns()<end) {
        int flags=0; long long ts=0;
        int n=d->readStream(rx,bufs,4096,flags,ts,200000);
        if(n==SOAPY_SDR_OVERFLOW){ overflows++; have=false; expect_t=0; continue; }
        if(n==SOAPY_SDR_TIMEOUT)continue;
        CHECK(n>0 && (flags&SOAPY_SDR_HAS_TIME));
        if(!first_t)first_t=ts;
        if(expect_t)worst_dt=std::max(worst_dt,std::fabs((double)(ts-expect_t)));
        expect_t=ts+(long long)n*4000; // 250 ksample/s
        for(int i=0;i<n;i++) {
            uint32_t v=(uint16_t)buf[2*i]|(uint32_t)(uint16_t)buf[2*i+1]<<16;
            if(have && v!=last+1)gaps++;
            last=v; have=true;
        }
        pairs+=(uint64_t)n;
    }
    std::fprintf(stderr,"RX %llu pairs, %u overflows, worst timestamp step error %.0f ns\n",
        (unsigned long long)pairs,overflows,worst_dt);
    CHECK(!gaps && pairs>100000 && first_t>=5000000000LL && worst_dt<1.0);
    // Retune while streaming: the stream restarts and reports one overflow.
    d->setFrequency(SOAPY_SDR_RX,0,915e6);
    bool saw_overflow=false; int got=0;
    for(int k=0;k<200 && got<10;k++) {
        int flags=0; long long ts=0;
        int n=d->readStream(rx,bufs,4096,flags,ts,200000);
        if(n==SOAPY_SDR_OVERFLOW)saw_overflow=true;
        else if(n>0)got++;
    }
    CHECK(saw_overflow && got==10);
    // A STEP change applies live; a BOOST change restarts the stream.
    d->setGain(SOAPY_SDR_RX,0,"STEP",9);
    for(int k=0;k<5;k++){ int flags=0; long long ts=0; CHECK(d->readStream(rx,bufs,4096,flags,ts,200000)>0); }
    d->setGain(SOAPY_SDR_RX,0,"BOOST",2);
    CHECK(d->getGain(SOAPY_SDR_RX,0,"BOOST")==2 && d->getGain(SOAPY_SDR_RX,0,"STEP")==9);
    saw_overflow=false; got=0;
    for(int k=0;k<200 && got<10;k++) {
        int flags=0; long long ts=0;
        int n=d->readStream(rx,bufs,4096,flags,ts,200000);
        if(n==SOAPY_SDR_OVERFLOW)saw_overflow=true;
        else if(n>0)got++;
    }
    CHECK(saw_overflow && got==10);
    d->deactivateStream(rx); d->closeStream(rx);

    // Raw route: source 0 IQ8 at 576000 pairs/s, counter in the int8 I byte.
    d->writeSetting("route","source0");
    d->setSampleRate(SOAPY_SDR_RX,0,576000);
    CHECK(d->getSampleRate(SOAPY_SDR_RX,0)==576000 && d->readSetting("route")=="source0");
    rx=d->setupStream(SOAPY_SDR_RX,SOAPY_SDR_CS16);
    CHECK(d->activateStream(rx)==0);
    pairs=0; gaps=0; have=false;
    end=now_ns()+300000000LL;
    while(now_ns()<end) {
        int flags=0; long long ts=0;
        int n=d->readStream(rx,bufs,4096,flags,ts,200000);
        if(n==SOAPY_SDR_OVERFLOW){ have=false; continue; }
        if(n<=0)continue;
        for(int i=0;i<n;i++) {
            uint32_t v=(uint8_t)(buf[2*i]/256);
            CHECK(buf[2*i]%256==0);
            if(have && v!=((last+1)&255))gaps++;
            last=v; have=true;
        }
        pairs+=(uint64_t)n;
    }
    std::fprintf(stderr,"source 0: %llu pairs in 0.3 s\n",(unsigned long long)pairs);
    CHECK(!gaps && pairs>50000);
    d->deactivateStream(rx); d->closeStream(rx);
    // Source 1: the rate selects the FSK filter code.
    d->writeSetting("route","source1");
    d->setSampleRate(SOAPY_SDR_RX,0,800000);
    CHECK(d->getSampleRate(SOAPY_SDR_RX,0)==800000 && d->readSetting("input_code")=="33");
    d->setSampleRate(SOAPY_SDR_RX,0,124000);
    CHECK(d->getSampleRate(SOAPY_SDR_RX,0)==125000 && d->readSetting("input_code")=="28");
    CHECK(d->listSampleRates(SOAPY_SDR_RX,0).size()>=10);
    d->writeSetting("route","source2");
    d->setSampleRate(SOAPY_SDR_RX,0,250000);

    // CF32 RX in range -1..1.
    rx=d->setupStream(SOAPY_SDR_RX,SOAPY_SDR_CF32);
    std::vector<float> fbuf(2*4096); void *fbufs[]={fbuf.data()};
    CHECK(d->activateStream(rx)==0);
    int n=0;
    for(int k=0;k<50 && n<=0;k++){ int flags; long long ts; n=d->readStream(rx,fbufs,4096,flags,ts,200000); }
    CHECK(n>0);
    for(int i=0;i<2*n;i++)CHECK(fbuf[(size_t)i]>=-1.0f && fbuf[(size_t)i]<1.0f);
    d->deactivateStream(rx); d->closeStream(rx);

    // TX: CF32 burst at 19200 S/s, then a timed burst, then over-long and over-range cases.
    d->setFrequency(SOAPY_SDR_TX,0,433.92e6);
    d->setSampleRate(SOAPY_SDR_TX,0,19200);
    d->setGain(SOAPY_SDR_TX,0,-9);
    SoapySDR::Stream *tx=d->setupStream(SOAPY_SDR_TX,SOAPY_SDR_CF32);
    CHECK(d->activateStream(tx)==0);
    std::vector<float> wave(2*9600);
    for(size_t i=0;i<9600;i++){ wave[2*i]=0.8f*std::cos(0.01f*i); wave[2*i+1]=0.8f*std::sin(0.01f*i); }
    const void *wbufs[]={wave.data()};
    int flags=0;
    CHECK(d->writeStream(tx,wbufs,4800,flags,0)==4800);
    const void *wbufs2[]={wave.data()+9600};
    flags=SOAPY_SDR_END_BURST;
    long long t0=now_ns();
    CHECK(d->writeStream(tx,wbufs2,4800,flags,0)==4800);
    CHECK(now_ns()-t0>=450000000LL);
    size_t mask=0; long long st=0;
    CHECK(d->readStreamStatus(tx,mask,flags,st,0)==0 && (flags&SOAPY_SDR_END_BURST));
    long long when=d->getHardwareTime("")+600000000LL;
    flags=SOAPY_SDR_HAS_TIME|SOAPY_SDR_END_BURST;
    CHECK(d->writeStream(tx,wbufs,960,flags,when)==960);
    CHECK(d->readStreamStatus(tx,mask,flags,st,0)==0 && st>=when && st<when+5000000LL);
    std::vector<int8_t> big(2*40000,10); const void *bb[]={big.data()};
    d->closeStream(tx);
    tx=d->setupStream(SOAPY_SDR_TX,SOAPY_SDR_CS8);
    flags=SOAPY_SDR_END_BURST;
    CHECK(d->writeStream(tx,bb,40000,flags,0)==SOAPY_SDR_STREAM_ERROR); // ROM limit 32767
    big[0]=57; flags=SOAPY_SDR_END_BURST;
    CHECK(d->writeStream(tx,bb,100,flags,0)==100); // radius above 56: sent, reported untested
    d->writeSetting("tx_checked","true");
    flags=SOAPY_SDR_END_BURST;
    CHECK(d->writeStream(tx,bb,100,flags,0)==SOAPY_SDR_STREAM_ERROR);
    d->writeSetting("tx_checked","false");
    d->closeStream(tx);
    SoapySDR::Device::unmake(d);
    std::puts("SoapySDR module over the simulator passed");
    return 0;
}
