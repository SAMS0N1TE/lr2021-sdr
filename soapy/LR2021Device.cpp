// SPDX-License-Identifier: MIT
// SoapySDR module for LR2021-SDR. Chip and protocol logic live in the C
// library (include/lr2021_host.h); this file maps the SoapySDR API onto it.
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.hpp>
#include <SoapySDR/Logger.hpp>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Time.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>
extern "C" {
#include "lr2021_host.h"
#include "lr2021_iq_dev.h"
#include "lr2021_iq_tx.h"
}

namespace {
const double TX_FULL_SCALE=56.0; // tested radius limit; CF32 magnitude 1.0 maps here
const long long NS=1000000000LL;

bool truthy(const std::string &v) { return v=="1" || v=="true" || v=="yes" || v=="on"; }
// FSK filter codes offered for source 1 (the 17..39 family holds the measured 28 and 33).
const uint8_t SOURCE1_CODES[]={17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39};
const char *const ROUTES[]={"source2","source0","source1","source6","raw0b"};
int route_index(const std::string &v)
{
    for(int i=0;i<5;i++)if(v==ROUTES[i])return i;
    throw std::invalid_argument("lr2021: route is source2, source0, source1, source6 or raw0b");
}
std::string text(const SoapySDR::Range &r)
{ return std::to_string(r.minimum())+".."+std::to_string(r.maximum()); }

class LR2021Device : public SoapySDR::Device {
public:
    explicit LR2021Device(const SoapySDR::Kwargs &args)
    {
        lr2021_host_args a;
        lr2021_host_args_init(&a);
        for(const auto &kv : args) {
            if(kv.first=="driver" || kv.first=="label" || kv.first=="tx_checked" || kv.first=="tx_simo" ||
               kv.first=="route" || kv.first=="input_code")continue;
            if(lr2021_host_args_set(&a,kv.first.c_str(),kv.second.c_str())<0)
                throw std::runtime_error("lr2021: bad value for "+kv.first);
        }
        auto u=args.find("tx_checked");
        checked=u!=args.end() && truthy(u->second);
        rx={154785000,31250,13,0,LR2021_IQ_PATH_AUTO,0,17,576000};
        auto ro=args.find("route");
        if(ro!=args.end())rx.route=(uint8_t)route_index(ro->second);
        auto ic=args.find("input_code");
        if(ic!=args.end())rx.input_code=(uint8_t)std::strtoul(ic->second.c_str(),nullptr,0);
        auto sm=args.find("tx_simo");
        simo=sm!=args.end() && truthy(sm->second);
        char err[256];
        host=lr2021_host_open(&a,err,sizeof(err));
        if(!host)throw std::runtime_error(std::string("lr2021: ")+err);
        const lr2021_bridge_hello *h=lr2021_host_hello(host);
        if(rx.route && !(h->caps&LR2021_BRIDGE_CAP_RAW_ROUTES))
            SoapySDR::logf(SOAPY_SDR_WARNING,"lr2021: firmware does not report raw-route support");
        apply_rx();
    }
    ~LR2021Device() override { lr2021_host_close(host); }

    std::string getDriverKey() const override { return "lr2021"; }
    std::string getHardwareKey() const override { return "LR2021"; }
    SoapySDR::Kwargs getHardwareInfo() const override
    {
        const lr2021_bridge_hello *h=lr2021_host_hello(host);
        static const char *names[]={"","serial","tcp","local","sim"};
        int b=lr2021_host_backend(host);
        return {{"firmware",h->name},{"id",h->id},{"backend",names[b>=0 && b<=4?b:0]},
            {"rom",std::to_string(h->rom_major)+"."+std::to_string(h->rom_minor)},
            {"protocol",std::to_string(h->version)},{"caps",std::to_string(h->caps)},
            {"tx_checked",checked?"true":"false"},{"route",ROUTES[rx.route]}};
    }
    size_t getNumChannels(const int) const override { return 1; }
    bool getFullDuplex(const int, const size_t) const override { return false; }

    // Antennas: input/PA path, automatic by frequency (LF below 1.5 GHz).
    std::vector<std::string> listAntennas(const int, const size_t) const override { return {"AUTO","LF","HF"}; }
    void setAntenna(const int dir, const size_t, const std::string &name) override
    {
        uint8_t v=name=="LF"?0:name=="HF"?1:name=="AUTO"?2:3;
        if(v==3)throw std::invalid_argument("lr2021: antenna is AUTO, LF or HF");
        std::lock_guard<std::mutex> l(mu);
        if(dir==SOAPY_SDR_TX){ tx_pa=v; return; }
        lr2021_iq_rx next=rx; next.path=v; set_rx_locked(next);
    }
    std::string getAntenna(const int dir, const size_t) const override
    {
        static const char *n[]={"LF","HF","AUTO"};
        return n[dir==SOAPY_SDR_TX?tx_pa:rx.path];
    }

    // Frequency
    SoapySDR::RangeList getFrequencyRange(const int dir, const size_t) const override
    {
        if(dir==SOAPY_SDR_TX && checked)return {SoapySDR::Range(LR2021_IQ_TX_MIN_HZ,LR2021_IQ_TX_MAX_HZ)};
        return {SoapySDR::Range(LR2021_IQ_MIN_HZ,LR2021_IQ_MAX_HZ)};
    }
    void setFrequency(const int dir, const size_t, const double f, const SoapySDR::Kwargs &) override
    {
        auto r=getFrequencyRange(dir,0).front();
        if(f<r.minimum() || f>r.maximum())throw std::out_of_range("lr2021: frequency outside "+text(r));
        std::lock_guard<std::mutex> l(mu);
        if(dir==SOAPY_SDR_TX){ tx_hz=(uint32_t)std::llround(f); return; }
        lr2021_iq_rx next=rx; next.frequency_hz=(uint32_t)std::llround(f); set_rx_locked(next);
    }
    double getFrequency(const int dir, const size_t) const override
    { return dir==SOAPY_SDR_TX?tx_hz:rx.frequency_hz; }

    // Sample rates. Source 2 is 4 x LoRa bandwidth (docs/protocol.md); sources 0
    // and 6 take an explicit output rate; source 1 runs at the input rate of its
    // FSK filter code. TX lists the measured rates.
    std::vector<double> listSampleRates(const int dir, const size_t) const override
    {
        if(dir==SOAPY_SDR_TX)return {4800,9600,19200,38400,76800,125000,250000,500000,1000000};
        if(rx.route==LR2021_IQ_ROUTE_SOURCE1) {
            std::vector<double> v;
            for(uint8_t c : SOURCE1_CODES)v.push_back(lr2021_iq_input_rate(c));
            std::sort(v.begin(),v.end());
            v.erase(std::unique(v.begin(),v.end()),v.end());
            return v;
        }
        if(rx.route && rx.route!=LR2021_IQ_ROUTE_RAW0B)return {50000,125000,250000,500000,576000,1000000};
        return {125000,250000,500000,1000000};
    }
    SoapySDR::RangeList getSampleRateRange(const int dir, const size_t ch) const override
    {
        if(dir==SOAPY_SDR_TX && !checked)return {SoapySDR::Range(1,1000000)};
        if(dir==SOAPY_SDR_RX && rx.route && rx.route!=LR2021_IQ_ROUTE_SOURCE1 && rx.route!=LR2021_IQ_ROUTE_RAW0B)
            return {SoapySDR::Range(1000,4000000)};
        SoapySDR::RangeList l;
        for(double r : listSampleRates(dir,ch))l.push_back(SoapySDR::Range(r,r));
        return l;
    }
    void setSampleRate(const int dir, const size_t, const double rate) override
    {
        std::lock_guard<std::mutex> l(mu);
        unsigned r=(unsigned)std::llround(rate);
        if(dir==SOAPY_SDR_TX) {
            if(!(checked ? lr2021_iq_tx_rate_ok(r) : r>=1 && r<=1000000))
                throw std::invalid_argument("lr2021: TX rate outside the allowed range");
            tx_rate=r; return;
        }
        lr2021_iq_rx next=rx;
        if(rx.route==LR2021_IQ_ROUTE_SOURCE1) {
            /* The closest rate an input code gives. */
            unsigned best=SOURCE1_CODES[0];
            for(uint8_t c : SOURCE1_CODES)
                if(std::llabs((long long)lr2021_iq_input_rate(c)-r)<std::llabs((long long)lr2021_iq_input_rate((uint8_t)best)-r))best=c;
            next.input_code=(uint8_t)best;
        } else if(rx.route && rx.route!=LR2021_IQ_ROUTE_RAW0B) {
            if(r<1000 || r>4000000)throw std::invalid_argument("lr2021: raw-route rate must be 1000..4000000");
            next.rate_hz=r;
        } else {
            uint32_t bw=lr2021_iq_bandwidth(r);
            if(!bw)throw std::invalid_argument("lr2021: source-2 rate must be 125000, 250000, 500000 or 1000000");
            next.bandwidth_hz=bw;
        }
        set_rx_locked(next);
    }
    double getSampleRate(const int dir, const size_t) const override
    { return dir==SOAPY_SDR_TX?tx_rate:lr2021_iq_rx_rate(&rx); }
    std::vector<double> listBandwidths(const int dir, const size_t ch) const override { return {getSampleRate(dir,ch)}; }
    double getBandwidth(const int dir, const size_t ch) const override { return getSampleRate(dir,ch); }

    // Gain. RX: STEP 1..13 and BOOST 0..7 (boost acts at steps 12 and 13), AGC = step 0.
    // TX: PA in configured dBm, SetTxParams in 0.5 dB steps.
    std::vector<std::string> listGains(const int dir, const size_t) const override
    { return dir==SOAPY_SDR_TX?std::vector<std::string>{"PA"}:std::vector<std::string>{"STEP","BOOST"}; }
    bool hasGainMode(const int dir, const size_t) const override { return dir==SOAPY_SDR_RX; }
    void setGainMode(const int dir, const size_t, const bool automatic) override
    {
        if(dir!=SOAPY_SDR_RX)return;
        std::lock_guard<std::mutex> l(mu);
        if(automatic && rx.gain_step)manual_step=rx.gain_step;
        set_gain_locked(automatic?0:manual_step,rx.boost);
    }
    bool getGainMode(const int dir, const size_t) const override { return dir==SOAPY_SDR_RX && !rx.gain_step; }
    SoapySDR::Range getGainRange(const int dir, const size_t) const override
    {
        if(dir==SOAPY_SDR_TX)return pa_range();
        return SoapySDR::Range(1,20,1);
    }
    SoapySDR::Range getGainRange(const int dir, const size_t, const std::string &name) const override
    {
        if(dir==SOAPY_SDR_TX)return pa_range();
        if(name=="BOOST")return SoapySDR::Range(0,7,1);
        return SoapySDR::Range(1,13,1);
    }
    void setGain(const int dir, const size_t, const double value) override
    {
        std::lock_guard<std::mutex> l(mu);
        if(dir==SOAPY_SDR_TX){ set_power_locked(value); return; }
        int g=(int)std::lround(std::min(20.0,std::max(1.0,value)));
        manual_step=(uint8_t)std::min(g,13);
        set_gain_locked(manual_step,(uint8_t)std::max(0,g-13));
    }
    void setGain(const int dir, const size_t, const std::string &name, const double value) override
    {
        std::lock_guard<std::mutex> l(mu);
        if(dir==SOAPY_SDR_TX){ set_power_locked(value); return; }
        int v=(int)std::lround(value);
        if(name=="BOOST")set_gain_locked(rx.gain_step,(uint8_t)std::min(7,std::max(0,v)));
        else { manual_step=(uint8_t)std::min(13,std::max(1,v)); set_gain_locked(manual_step,rx.boost); }
    }
    double getGain(const int dir, const size_t) const override
    { return dir==SOAPY_SDR_TX?tx_power/2.0:(rx.gain_step?rx.gain_step:13)+rx.boost; }
    double getGain(const int dir, const size_t, const std::string &name) const override
    {
        if(dir==SOAPY_SDR_TX)return tx_power/2.0;
        return name=="BOOST"?rx.boost:rx.gain_step;
    }

    // Time: device clock in microseconds plus a host-side offset.
    bool hasHardwareTime(const std::string &what) const override { return what.empty(); }
    long long getHardwareTime(const std::string &) const override
    {
        uint64_t us=0;
        if(lr2021_host_time(host,&us))throw std::runtime_error("lr2021: time request failed");
        return (long long)us*1000+time_offset;
    }
    void setHardwareTime(const long long t, const std::string &what) override
    {
        if(!what.empty())throw std::invalid_argument("lr2021: only the default time source");
        uint64_t us=0;
        if(lr2021_host_time(host,&us))throw std::runtime_error("lr2021: time request failed");
        time_offset=t-(long long)us*1000;
    }

    std::vector<std::string> listSensors(const int dir, const size_t) const override
    {
        if(dir!=SOAPY_SDR_RX || !(lr2021_host_hello(host)->caps&LR2021_BRIDGE_CAP_GAIN_READBACK))return {};
        return {"chip_gain_step","chip_gain_value"};
    }
    SoapySDR::ArgInfo getSensorInfo(const int, const size_t, const std::string &key) const override
    {
        SoapySDR::ArgInfo i;
        i.key=key; i.type=SoapySDR::ArgInfo::INT;
        if(key=="chip_gain_step") {
            i.name="Chip gain step"; i.range=SoapySDR::Range(1,13);
            i.description="Gain step the LR2021 is using, 1 to 13, read from the chip";
        } else if(key=="chip_gain_value") {
            i.name="Chip gain value"; i.range=SoapySDR::Range(0,511);
            i.description="Raw gain value of the current step, unscaled";
        }
        return i;
    }
    std::string readSensor(const int dir, const size_t, const std::string &key) const override
    {
        if(dir!=SOAPY_SDR_RX || (key!="chip_gain_step" && key!="chip_gain_value"))
            throw std::invalid_argument("lr2021: unknown sensor "+key);
        lr2021_bridge_gain g;
        if(lr2021_host_get_gain(host,&g))throw std::runtime_error("lr2021: gain readback failed");
        return std::to_string(key=="chip_gain_step"?(unsigned)g.step:(unsigned)g.gain_value);
    }

    // Settings
    SoapySDR::ArgInfoList getSettingInfo() const override
    {
        SoapySDR::ArgInfo u;
        u.key="tx_checked"; u.value=checked?"true":"false"; u.name="Checked TX";
        u.type=SoapySDR::ArgInfo::BOOL;
        u.description="Refuse TX bursts outside the measured contract instead of reporting them";
        SoapySDR::ArgInfo r;
        r.key="route"; r.value=ROUTES[rx.route]; r.name="Receive route";
        r.type=SoapySDR::ArgInfo::STRING; r.options={"source2","source0","source1","source6","raw0b"};
        r.description="source2: LoRa IQ16 at 4 x bandwidth; source0/1: IQ8, source6: gain-tagged IQ6, at the set rate; raw0b: IQ8 in packet type 0x0B at 4 x bandwidth";
        SoapySDR::ArgInfo c;
        c.key="input_code"; c.value=std::to_string(rx.input_code); c.name="Raw-route input code";
        c.type=SoapySDR::ArgInfo::INT;
        c.description="FSK RX filter code for the raw routes (17 measured with source 0)";
        SoapySDR::ArgInfo m;
        m.key="tx_simo"; m.value=simo?"true":"false"; m.name="TX on SIMO";
        m.type=SoapySDR::ArgInfo::BOOL;
        m.description="Run TX bursts with the SIMO DC-DC supplying the PA instead of the LDO (experimental)";
        return {u,m,r,c};
    }
    void writeSetting(const std::string &key, const std::string &value) override
    {
        if(key=="tx_checked")checked=truthy(value);
        if(key=="tx_simo")simo=truthy(value);
        if(key=="route" || key=="input_code") {
            std::lock_guard<std::mutex> l(mu);
            lr2021_iq_rx next=rx;
            if(key=="route")next.route=(uint8_t)route_index(value);
            else next.input_code=(uint8_t)std::strtoul(value.c_str(),nullptr,0);
            set_rx_locked(next);
        }
    }
    std::string readSetting(const std::string &key) const override
    {
        if(key=="tx_checked")return checked?"true":"false";
        if(key=="route")return ROUTES[rx.route];
        if(key=="input_code")return std::to_string(rx.input_code);
        if(key=="tx_simo")return simo?"true":"false";
        return "";
    }

    // Streams
    std::vector<std::string> getStreamFormats(const int dir, const size_t) const override
    {
        if(dir==SOAPY_SDR_TX)return {SOAPY_SDR_CS8,SOAPY_SDR_CS16,SOAPY_SDR_CF32};
        return {SOAPY_SDR_CS16,SOAPY_SDR_CF32};
    }
    std::string getNativeStreamFormat(const int dir, const size_t, double &full) const override
    {
        if(dir==SOAPY_SDR_TX){ full=TX_FULL_SCALE; return SOAPY_SDR_CS8; }
        full=32768; return SOAPY_SDR_CS16;
    }
    SoapySDR::ArgInfoList getStreamArgsInfo(const int dir, const size_t) const override
    {
        if(dir!=SOAPY_SDR_TX)return {};
        SoapySDR::ArgInfo r;
        r.key="repeats"; r.value="1"; r.name="Repeats"; r.type=SoapySDR::ArgInfo::INT;
        r.description="Transmit each burst this many times in one run (1..16 tested)";
        return {r};
    }
    SoapySDR::Stream *setupStream(const int dir, const std::string &format, const std::vector<size_t> &ch,
                                  const SoapySDR::Kwargs &args) override
    {
        if(ch.size()>1 || (ch.size()==1 && ch[0]))throw std::invalid_argument("lr2021: one channel");
        auto formats=getStreamFormats(dir,0);
        if(std::find(formats.begin(),formats.end(),format)==formats.end())
            throw std::invalid_argument("lr2021: format "+format+" not supported");
        Stream *s=new Stream;
        s->dir=dir; s->cf32=format==SOAPY_SDR_CF32; s->cs8=format==SOAPY_SDR_CS8;
        auto r=args.find("repeats");
        if(r!=args.end())s->repeats=(unsigned)std::max(1L,std::strtol(r->second.c_str(),nullptr,10));
        return reinterpret_cast<SoapySDR::Stream *>(s);
    }
    void closeStream(SoapySDR::Stream *stream) override
    {
        Stream *s=reinterpret_cast<Stream *>(stream);
        if(s->dir==SOAPY_SDR_RX && s->active)deactivateStream(stream,0,0);
        delete s;
    }
    size_t getStreamMTU(SoapySDR::Stream *stream) const override
    { return reinterpret_cast<Stream *>(stream)->dir==SOAPY_SDR_TX?LR2021_IQ_TX_MAX_SAMPLES:4096; }
    int activateStream(SoapySDR::Stream *stream, const int, const long long, const size_t) override
    {
        Stream *s=reinterpret_cast<Stream *>(stream);
        std::lock_guard<std::mutex> l(mu);
        s->active=true;
        if(s->dir==SOAPY_SDR_TX){ s->burst.clear(); s->have_time=false; return 0; }
        rx_wanted=true;
        return lr2021_host_rx_start(host,0)?SOAPY_SDR_STREAM_ERROR:0;
    }
    int deactivateStream(SoapySDR::Stream *stream, const int, const long long) override
    {
        Stream *s=reinterpret_cast<Stream *>(stream);
        std::lock_guard<std::mutex> l(mu);
        s->active=false;
        if(s->dir==SOAPY_SDR_RX){ rx_wanted=false; lr2021_host_rx_stop(host,nullptr); }
        return 0;
    }
    int readStream(SoapySDR::Stream *stream, void *const *buffs, const size_t n, int &flags,
                   long long &timeNs, const long timeoutUs) override
    {
        Stream *s=reinterpret_cast<Stream *>(stream);
        if(s->dir!=SOAPY_SDR_RX)return SOAPY_SDR_NOT_SUPPORTED;
        flags=0;
        size_t want=std::min(n,(size_t)4096);
        lr2021_host_rx_meta m{};
        long got;
        if(s->carry_pairs) { // data held back while the overflow was reported
            got=(long)std::min(want,s->carry_pairs);
            m=s->carry_meta;
            std::memcpy(s->iq.data(),s->carry.data(),(size_t)got*4);
            s->carry_pairs-=(size_t)got;
            if(s->carry_pairs)std::memmove(s->carry.data(),s->carry.data()+2*got,s->carry_pairs*4);
            s->carry_meta.first_pair+=(uint64_t)got;
            s->carry_meta.time_ns+=(uint64_t)got*1000000000ull/std::max(1u,m.rate_hz);
        } else {
            s->iq.resize(2*want);
            got=lr2021_host_rx_read(host,s->iq.data(),want,(unsigned)std::max(0L,timeoutUs),&m);
            if(got<0)return SOAPY_SDR_STREAM_ERROR;
            if(m.ended && !got) { // radio error: restart and report the gap
                std::lock_guard<std::mutex> l(mu);
                if(rx_wanted && !lr2021_host_rx_active(host))lr2021_host_rx_start(host,0);
                return SOAPY_SDR_OVERFLOW;
            }
            if(!got)return SOAPY_SDR_TIMEOUT;
            bool gap=m.discontinuity || (s->generation && m.generation!=s->generation);
            s->generation=m.generation;
            if(gap) {
                s->carry.assign(s->iq.begin(),s->iq.begin()+2*got); s->carry_pairs=(size_t)got; s->carry_meta=m;
                s->carry_meta.discontinuity=false;
                return SOAPY_SDR_OVERFLOW;
            }
        }
        if(s->cf32) {
            float *out=static_cast<float *>(buffs[0]);
            for(long i=0;i<2*got;i++)out[i]=s->iq[(size_t)i]*(1.0f/32768.0f);
        } else std::memcpy(buffs[0],s->iq.data(),(size_t)got*4);
        flags|=SOAPY_SDR_HAS_TIME;
        timeNs=(long long)m.time_ns+time_offset;
        return (int)got;
    }
    int writeStream(SoapySDR::Stream *stream, const void *const *buffs, const size_t n, int &flags,
                    const long long timeNs, const long) override
    {
        Stream *s=reinterpret_cast<Stream *>(stream);
        if(s->dir!=SOAPY_SDR_TX)return SOAPY_SDR_NOT_SUPPORTED;
        if(s->burst.empty() && (flags&SOAPY_SDR_HAS_TIME)){ s->have_time=true; s->time_ns=timeNs; }
        if(s->burst.size()/2+n>LR2021_IQ_TX_MAX_SAMPLES) {
            s->burst.clear(); s->status=SOAPY_SDR_STREAM_ERROR; s->status_ready=true;
            SoapySDR::logf(SOAPY_SDR_ERROR,"lr2021: a burst holds at most %u samples",LR2021_IQ_TX_MAX_SAMPLES);
            return SOAPY_SDR_STREAM_ERROR;
        }
        size_t base=s->burst.size();
        s->burst.resize(base+2*n);
        int8_t *out=s->burst.data()+base;
        for(size_t i=0;i<n;i++) {
            double a,b;
            if(s->cs8){ const int8_t *in=static_cast<const int8_t *>(buffs[0]); out[2*i]=in[2*i]; out[2*i+1]=in[2*i+1]; continue; }
            if(s->cf32){ const float *in=static_cast<const float *>(buffs[0]); a=in[2*i]*TX_FULL_SCALE; b=in[2*i+1]*TX_FULL_SCALE; }
            else { const int16_t *in=static_cast<const int16_t *>(buffs[0]); a=in[2*i]*TX_FULL_SCALE/32768.0; b=in[2*i+1]*TX_FULL_SCALE/32768.0; }
            double r=std::hypot(a,b);
            if(r>TX_FULL_SCALE){ a*=TX_FULL_SCALE/r; b*=TX_FULL_SCALE/r; s->clipped++; }
            out[2*i]=(int8_t)std::lround(a); out[2*i+1]=(int8_t)std::lround(b);
            /* rounding can push the radius just past 56 */
            while(out[2*i]*out[2*i]+out[2*i+1]*out[2*i+1]>56*56)
                out[2*i]=(int8_t)(out[2*i]-(out[2*i]>0)+(out[2*i]<0));
        }
        if(!(flags&SOAPY_SDR_END_BURST))return (int)n;
        int st=transmit(s);
        return st?st:(int)n;
    }
    int readStreamStatus(SoapySDR::Stream *stream, size_t &mask, int &flags, long long &timeNs, const long timeoutUs) override
    {
        Stream *s=reinterpret_cast<Stream *>(stream);
        if(s->dir!=SOAPY_SDR_TX)return SOAPY_SDR_NOT_SUPPORTED;
        (void)timeoutUs; // bursts complete inside writeStream, so nothing is ever pending
        if(!s->status_ready)return SOAPY_SDR_TIMEOUT;
        s->status_ready=false; mask=1; flags=s->status_flags; timeNs=s->status_time;
        return s->status;
    }

private:
    struct Stream {
        int dir=0; bool cf32=false, cs8=false, active=false;
        unsigned repeats=1;
        std::vector<int16_t> iq, carry; size_t carry_pairs=0; lr2021_host_rx_meta carry_meta{};
        uint32_t generation=0;
        std::vector<int8_t> burst; bool have_time=false; long long time_ns=0; unsigned long clipped=0;
        bool status_ready=false; int status=0, status_flags=0; long long status_time=0;
    };
    lr2021_host *host=nullptr;
    mutable std::mutex mu;
    lr2021_iq_rx rx{};
    uint8_t manual_step=13, tx_pa=LR2021_IQ_PA_AUTO;
    uint32_t tx_hz=433920000, tx_rate=125000;
    int tx_power=LR2021_IQ_TX_DEFAULT_POWER;
    bool checked=false, simo=false, rx_wanted=false;
    long long time_offset=0;

    SoapySDR::Range pa_range() const
    {
        if(checked)return SoapySDR::Range(LR2021_IQ_TX_MIN_POWER/2.0,LR2021_IQ_TX_MAX_POWER/2.0,0.5);
        bool hf=tx_pa==1 || (tx_pa==2 && tx_hz>=LR2021_IQ_HF_HZ);
        return hf?SoapySDR::Range(-19.5,12,0.5):SoapySDR::Range(-9.5,22,0.5);
    }
    void set_power_locked(double dbm)
    {
        auto r=pa_range();
        if(dbm<r.minimum() || dbm>r.maximum())throw std::out_of_range("lr2021: TX power outside "+text(r)+" dBm");
        tx_power=(int)std::lround(dbm*2);
    }
    void apply_rx()
    {
        lr2021_bridge_rx m={rx.frequency_hz,rx.bandwidth_hz,rx.gain_step,rx.boost,rx.path,rx.route,rx.input_code,rx.rate_hz};
        lr2021_bridge_rx_ack ack;
        int e=lr2021_host_set_rx(host,&m,&ack);
        if(e)throw std::runtime_error(std::string("lr2021: receive configuration: ")+lr2021_host_strerror(e));
        if(ack.radio_errors)SoapySDR::logf(SOAPY_SDR_INFO,"lr2021: radio errors 0x%04X after tuning",ack.radio_errors);
    }
    // Retuning restarts the stream: the device refuses SET_RX while streaming.
    void set_rx_locked(const lr2021_iq_rx &next)
    {
        bool streaming=lr2021_host_rx_active(host);
        if(streaming)lr2021_host_rx_stop(host,nullptr);
        lr2021_iq_rx old=rx; rx=next;
        try { apply_rx(); } catch(...) { rx=old; if(streaming)lr2021_host_rx_start(host,0); throw; }
        if(streaming)lr2021_host_rx_start(host,0);
    }
    void set_gain_locked(uint8_t step, uint8_t boost)
    {
        int e=lr2021_host_set_gain(host,step,boost);
        if(e==LR2021_BRIDGE_EBUSY && lr2021_host_rx_active(host)) {
            /* A boost change needs SetRxPath, which the ROM refuses in RX:
             * restart the stream around it (one SOAPY_SDR_OVERFLOW). */
            lr2021_host_rx_stop(host,nullptr);
            e=lr2021_host_set_gain(host,step,boost);
            lr2021_host_rx_start(host,0);
        }
        if(e)throw std::runtime_error(std::string("lr2021: gain: ")+lr2021_host_strerror(e));
        rx.gain_step=step; rx.boost=boost;
    }
    int transmit(Stream *s)
    {
        std::vector<int8_t> burst; burst.swap(s->burst);
        size_t pairs=burst.size()/2;
        s->status_ready=true; s->status_flags=SOAPY_SDR_END_BURST; s->status_time=0;
        if(!pairs){ s->status=0; return 0; }
        if(s->clipped)SoapySDR::logf(SOAPY_SDR_WARNING,"lr2021: %lu samples limited to radius 56",s->clipped);
        s->clipped=0;
        std::lock_guard<std::mutex> l(mu);
        lr2021_bridge_tx_begin p{};
        p.rate_hz=tx_rate; p.frequency_hz=tx_hz; p.power_half_db=(int8_t)tx_power;
        p.repeats=(uint8_t)std::min(255u,s->repeats); p.pa=tx_pa;
        p.flags=(uint8_t)((checked?LR2021_IQ_TX_CHECKED:0)|(simo?LR2021_IQ_TX_SIMO:0));
        if(s->have_time)p.start_time=(uint64_t)std::max(0LL,(s->time_ns-time_offset)/1000);
        s->have_time=false;
        // Half duplex: stop receive for the burst and resume it afterwards.
        bool resume=lr2021_host_rx_active(host);
        if(resume)lr2021_host_rx_stop(host,nullptr);
        lr2021_bridge_tx_end end{};
        int e=lr2021_host_tx(host,burst.data(),pairs,&p,&end);
        if(resume)lr2021_host_rx_start(host,0);
        s->status_time=(long long)end.start_time*1000+time_offset;
        if(end.untested)SoapySDR::logf(SOAPY_SDR_WARNING,"lr2021: burst outside the tested TX contract (bits 0x%X)",end.untested);
        if(!e){ s->status=0; return 0; }
        SoapySDR::logf(SOAPY_SDR_ERROR,"lr2021: transmit failed: %s",lr2021_host_strerror(e));
        s->status=(end.tx_flags&0x20)?SOAPY_SDR_UNDERFLOW:SOAPY_SDR_STREAM_ERROR;
        return s->status;
    }
};

SoapySDR::KwargsList find(const SoapySDR::Kwargs &args)
{
    SoapySDR::KwargsList out;
    auto d=args.find("driver");
    if(d!=args.end() && d->second!="lr2021")return out;
    bool explicit_target=args.count("serial") || args.count("host") || args.count("spidev") || args.count("sim");
    if(explicit_target) {
        SoapySDR::Kwargs k=args; k["driver"]="lr2021";
        std::string what=args.count("sim")?"simulator":args.count("host")?args.at("host"):
            args.count("spidev")?args.at("spidev"):args.at("serial");
        k["label"]="LR2021 "+what;
        if(args.count("probe") && truthy(args.at("probe"))) {
            try { LR2021Device dev(args); k["id"]=dev.getHardwareInfo()["id"]; }
            catch(const std::exception &) { return out; }
        }
        k.erase("probe");
        out.push_back(k);
        return out;
    }
    // Ports are listed by USB ID only; they are not opened, because opening
    // can reset a board. Use probe=1 with serial= to verify one.
    char paths[16][256];
    size_t n=lr2021_host_list_serial(paths,16);
    for(size_t i=0;i<n;i++)
        out.push_back({{"driver","lr2021"},{"serial",paths[i]},{"label",std::string("LR2021 bridge? ")+paths[i]}});
    return out;
}
SoapySDR::Device *make(const SoapySDR::Kwargs &args) { return new LR2021Device(args); }
SoapySDR::Registry registration("lr2021",&find,&make,SOAPY_SDR_ABI_VERSION);
}
