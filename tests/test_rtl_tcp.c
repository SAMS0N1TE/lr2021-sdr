/* SPDX-License-Identifier: MIT */
/* rtl_tcp server against the simulator: the mappings, the command line, then
 * a real server on a TCP bridge simulator with a test client that checks the
 * header, sends rtl_tcp commands, watches the simulated chip take them and
 * measures the u8 stream (rate, tone offset, level). Real time, about 6 s. */
#define _DEFAULT_SOURCE
#include "../src/host/host_internal.h"
#include "lr2021_sim.h"
#include "lr2021_tcp.h"
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
#define TONE_HZ 433920000u
#define PI 3.14159265358979323846

static uint64_t clock_us(void *c) { (void)c; return lrh_now_us(); }
static void sleep_cb(void *c, unsigned us) { (void)c; lrh_sleep_us(us); }
static int configure(void *c, const lr2021_iq_rx *rx, uint16_t *errors)
{ lr2021_iq_dev *d=c; int e=lr2021_iq_dev_configure(d,rx); *errors=d->errors; return e; }
typedef struct { lrh_fd fd; lr2021_sim sim; lr2021_iq_dev dev; } served;
static served bridge;
/* The bridge simulator: one TCP connection, a tone at TONE_HZ. */
static void *serve(void *c)
{
    served *s=c;
    lrh_fd ls=s->fd;
    s->fd=lrh_tcp_accept(ls,10000,NULL,0); lrh_close(ls);
    if(s->fd==LRH_NO_FD)return NULL;
    lr2021_sim_init(&s->sim);
    s->sim.clock=clock_us; s->sim.sleep=sleep_cb; s->sim.pattern=LR2021_SIM_TONE;
    s->sim.tone_hz=TONE_HZ; s->sim.stall_tolerant=true;
    lr2021_iq_bus bus=lr2021_sim_bus(&s->sim);
    const lr2021_iq_rx rx={154785000,31250,13,0,2,0,0,0};
    if(!lr2021_iq_dev_open(&s->dev,&bus,NULL,&rx)) {
        lr2021_bridge_radio radio={&s->dev,&s->dev.hal,NULL,configure,rx,1,24,
            LR2021_BRIDGE_CAP_RX|LR2021_BRIDGE_CAP_LIVE_GAIN|LR2021_BRIDGE_CAP_RATE_250K,"t","rtl-tcp-test"};
        lrh_server *srv=lrh_server_start(s->fd,&radio,0);
        if(srv){ lrh_server_wait(srv); lrh_server_free(srv); }
    }
    lrh_close(s->fd);
    return NULL;
}
static void *run(void *c) { lr2021_tcp_serve(c); return NULL; }

/* The simulator is driven from the server's threads; read its fields through
 * volatile and wait for them to change. */
static uint32_t sim_freq(void) { return *(volatile uint32_t *)&bridge.sim.freq; }
static unsigned sim_step(void) { return *(volatile uint8_t *)&bridge.sim.step; }
static unsigned sim_boost(void) { return *(volatile uint8_t *)&bridge.sim.boost; }
static unsigned sim_bw(void) { return *(volatile uint8_t *)&bridge.sim.lora_bw; }
static bool wait_sim(uint32_t freq, unsigned step, unsigned boost, unsigned bw)
{
    uint64_t end=lrh_now_us()+3000000u;
    while(lrh_now_us()<end) {
        if(sim_freq()==freq && sim_step()==step && (step<12 || sim_boost()==boost) && sim_bw()==bw)return true;
        lrh_sleep_us(5000);
    }
    fprintf(stderr,"  simulator: %u Hz step %u boost %u bw %u; wanted %u Hz step %u boost %u bw %u\n",
        (unsigned)sim_freq(),sim_step(),sim_boost(),sim_bw(),(unsigned)freq,step,boost,bw);
    return false;
}
static int cmd(lrh_fd fd, uint8_t c, uint32_t v)
{
    uint8_t b[5]={c,(uint8_t)(v>>24),(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v};
    return lrh_write_all(fd,b,5);
}
static long read_for(lrh_fd fd, uint8_t *buf, size_t cap, unsigned ms)
{
    uint64_t end=lrh_now_us()+ms*1000u;
    size_t got=0;
    while(lrh_now_us()<end) {
        if(lrh_poll_read(fd,20)!=1)continue;
        static uint8_t scratch[65536];
        long n=lrh_read(fd,got<cap?buf+got:scratch,got<cap?cap-got:sizeof(scratch));
        if(n<=0)return n==0?-1:n;
        if(got<cap)got+=(size_t)n;
        if(got==cap)break;
    }
    return (long)got;
}
/* Bytes per second, mean amplitude and tone offset of u8 pairs. */
static void measure(const uint8_t *b, size_t pairs, double *amplitude, double *offset_hz)
{
    double a=0, turn=0, last=0;
    for(size_t i=0;i<pairs;i++) {
        double x=b[2*i]-127.5, y=b[2*i+1]-127.5, p=atan2(y,x);
        a+=sqrt(x*x+y*y);
        if(i){ double d=p-last; while(d>PI)d-=2*PI; while(d<-PI)d+=2*PI; turn+=d; }
        last=p;
    }
    *amplitude=a/(double)pairs;
    *offset_hz=turn/(double)(pairs-1)*250000.0/(2*PI);
}
static int stream_check(lrh_fd fd, double min_amp, double max_amp, double offset)
{
    static uint8_t buf[400000];
    CHECK(read_for(fd,buf,sizeof(buf),250)>0); /* settle after the change */
    uint64_t t0=lrh_now_us();
    long n=read_for(fd,buf,sizeof(buf),600);
    double rate=(double)n*1e6/(double)(lrh_now_us()-t0)/2.0, amp, off;
    CHECK(n>100000);
    measure(buf,(size_t)n/2,&amp,&off);
    fprintf(stderr,"  %.0f pairs/s, amplitude %.1f, tone at %+.0f Hz\n",rate,amp,off);
    CHECK(rate>200000 && rate<300000);
    CHECK(amp>=min_amp && amp<=max_amp);
    CHECK(fabs(off-offset)<200);
    return 0;
}

static int mappings(void)
{
    CHECK(lr2021_tcp_snap_rate(2048000,true)==250000 && lr2021_tcp_snap_rate(250000,true)==250000);
    CHECK(lr2021_tcp_snap_rate(187500,true)==250000 && lr2021_tcp_snap_rate(187499,true)==125000);
    CHECK(lr2021_tcp_snap_rate(1,true)==125000 && lr2021_tcp_snap_rate(2400000,false)==125000);
    CHECK(lr2021_tcp_gain_index(0)==0 && lr2021_tcp_gain_index(496)==28 && lr2021_tcp_gain_index(1000)==28);
    CHECK(lr2021_tcp_gain_index(-50)==0 && lr2021_tcp_gain_index(200)==11 && lr2021_tcp_gain_index(205)==12 && lr2021_tcp_gain_index(150)==8);
    unsigned prev=0;
    for(unsigned i=0;i<LR2021_TCP_GAINS;i++) {
        unsigned l=lr2021_tcp_index_level(i);
        CHECK(l>=prev && l<=prev+1u+(i==0) && l>=1 && l<=20);
        prev=l;
    }
    CHECK(lr2021_tcp_index_level(0)==1 && lr2021_tcp_index_level(14)==11 && lr2021_tcp_index_level(28)==20);
    CHECK(lr2021_tcp_index_level(99)==20);
    uint8_t s,b;
    lr2021_tcp_level_gain(1,&s,&b); CHECK(s==1 && b==0);
    lr2021_tcp_level_gain(13,&s,&b); CHECK(s==13 && b==0);
    lr2021_tcp_level_gain(14,&s,&b); CHECK(s==13 && b==1);
    lr2021_tcp_level_gain(20,&s,&b); CHECK(s==13 && b==7);
    CHECK(lr2021_tcp_correct(433920000,0)==433920000);
    CHECK(lr2021_tcp_correct(1000000000,10)==999990000 && lr2021_tcp_correct(1000000000,-10)==1000010000);
    int16_t iq[10]={0,-1,31,32,4064,-4096,4096,-4097,32767,-32768};
    uint8_t out[10];
    CHECK(lr2021_tcp_convert(iq,out,5,1.0/32)==4);
    const uint8_t want[10]={128,127,128,129,255,0,255,0,255,0};
    CHECK(!memcmp(out,want,10));
    CHECK(lr2021_tcp_convert(iq,out,5,1.0/256)==0 && out[8]==255 && out[9]==0 && out[0]==128);
    return 0;
}
static int command_line(void)
{
    lr2021_tcp_config c; char err[200];
    lr2021_tcp_config_init(&c);
    char *ok[]={"lr2021_tcp","-d","host=10.0.0.2,port=5000,format=cs12","-d","rfsw=8:2,10:0","-a","0.0.0.0",
        "-p","4321","-f","145.5M","-s","125k","-g","auto","-P","-12","--antenna","lf","--shift","4","-q"};
    CHECK(!lr2021_tcp_parse(&c,(int)(sizeof(ok)/sizeof(ok[0])),ok,err,sizeof(err)));
    CHECK(!strcmp(c.device.host,"10.0.0.2") && c.device.port==5000 && c.device.format==LR2021_BRIDGE_CS12);
    CHECK(!strcmp(c.device.rfsw,"8:2,10:0") && !strcmp(c.listen,"0.0.0.0") && c.port==4321);
    CHECK(c.frequency_hz==145500000 && c.rate_hz==125000 && c.level==0 && c.ppm==-12 && c.path==0);
    CHECK(c.scale==1.0/16 && c.quiet);
    char *help[]={"lr2021_tcp","--help"};
    CHECK(lr2021_tcp_parse(&c,2,help,err,sizeof(err))==1);
    char *bad[][3]={{"lr2021_tcp","-g","21"},{"lr2021_tcp","-d","nope=1"},{"lr2021_tcp","-f","12x"},
        {"lr2021_tcp","--shift","16"},{"lr2021_tcp","-d","sim=maybe"},{"lr2021_tcp","-p","70000"}};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++) {
        lr2021_tcp_config_init(&c);
        CHECK(lr2021_tcp_parse(&c,3,bad[i],err,sizeof(err))==-1);
    }
    lr2021_tcp_config_init(&c);
    char *none[]={"lr2021_tcp","-p","1"};
    CHECK(lr2021_tcp_parse(&c,3,none,err,sizeof(err))==-1 && strstr(err,"no device"));
    return 0;
}

int main(void)
{
#ifdef SIGPIPE
    signal(SIGPIPE,SIG_IGN);
#endif
    CHECK(!mappings());
    CHECK(!command_line());

    char err[200];
    lrh_fd ls=lrh_tcp_listen("127.0.0.1",0,err,sizeof(err));
    CHECK(ls!=LRH_NO_FD);
    unsigned bridge_port=lrh_tcp_port(ls);
    bridge.fd=ls;
    lrh_thread bt; CHECK(!lrh_thread_start(&bt,serve,&bridge));

    lr2021_tcp_config cfg;
    lr2021_tcp_config_init(&cfg);
    snprintf(cfg.device.host,sizeof(cfg.device.host),"127.0.0.1"); cfg.device.port=bridge_port;
    cfg.port=0; cfg.frequency_hz=TONE_HZ-30000; cfg.rate_hz=125000;
    CHECK((cfg.log=fopen("test_rtl_tcp.log","w+")));
    lr2021_tcp *t=lr2021_tcp_open(&cfg,err,sizeof(err));
    if(!t)fprintf(stderr,"%s\n",err);
    CHECK(t);
    unsigned port=lr2021_tcp_port(t);
    CHECK(port);
    lrh_thread st; CHECK(!lrh_thread_start(&st,run,t));

    for(int round=0;round<2;round++) {
        lrh_fd fd=lrh_tcp_connect("127.0.0.1",port,2000,err,sizeof(err));
        CHECK(fd!=LRH_NO_FD);
        uint8_t h[12];
        CHECK(read_for(fd,h,12,1000)>=12);
        CHECK(!memcmp(h,"RTL0",4) && !memcmp(h+4,"\0\0\0\5\0\0\0\35",8)); /* R820T, 29 gains */
        if(round==1) {
            /* Settings persist between clients. */
            fprintf(stderr,"reconnect\n");
            CHECK(wait_sim(TONE_HZ-10000,0,0,3));
            CHECK(!stream_check(fd,100,160,10000));
            /* A second client is refused while this one is served. */
            lrh_fd other=lrh_tcp_connect("127.0.0.1",port,2000,err,sizeof(err));
            CHECK(other!=LRH_NO_FD);
            uint8_t x; CHECK(lrh_poll_read(other,3000)==1 && lrh_read(other,&x,1)<=0);
            lrh_close(other);
            CHECK(!stream_check(fd,100,160,10000));
            /* The radio session fails (FIFO overflow) until the fault is
             * cleared: the server restarts the stream and reports it. */
            fprintf(stderr,"device-side overflow\n");
            *(volatile uint64_t *)&bridge.sim.overflow_after=*(volatile uint64_t *)&bridge.sim.rx_pairs+20000u;
            static uint8_t junk[300000];
            CHECK(read_for(fd,junk,sizeof(junk),400)>0);
            *(volatile uint64_t *)&bridge.sim.overflow_after=0;
            CHECK(!stream_check(fd,100,160,10000));
            lrh_close(fd);
            break;
        }
        /* What clients send on connect: a 2.4 MS/s rate becomes 250000. */
        fprintf(stderr,"tune, rate, gain index\n");
        CHECK(!cmd(fd,LR2021_TCP_SET_FREQ,TONE_HZ-10000) && !cmd(fd,LR2021_TCP_SET_RATE,2400000));
        CHECK(!cmd(fd,LR2021_TCP_SET_GAIN_MODE,1) && !cmd(fd,LR2021_TCP_SET_GAIN_INDEX,22));
        CHECK(!cmd(fd,LR2021_TCP_SET_PPM,0) && !cmd(fd,LR2021_TCP_SET_AGC,0) && !cmd(fd,LR2021_TCP_SET_BIAS_TEE,0));
        CHECK(wait_sim(TONE_HZ-10000,13,3,3));          /* index 22: level 16 */
        CHECK(!stream_check(fd,100,160,10000));         /* 4000 counts >> 5 = 125 */
        fprintf(stderr,"gain in tenths of a dB\n");
        CHECK(!cmd(fd,LR2021_TCP_SET_GAIN,0));
        CHECK(wait_sim(TONE_HZ-10000,1,0,3));           /* -12 dB in the model */
        CHECK(!stream_check(fd,20,45,10000));
        fprintf(stderr,"retune and frequency correction\n");
        CHECK(!cmd(fd,LR2021_TCP_SET_GAIN,280) && !cmd(fd,LR2021_TCP_SET_FREQ,TONE_HZ+20000));
        CHECK(wait_sim(TONE_HZ+20000,11,0,3));          /* 28.0 dB: index 15, level 11 */
        CHECK(!stream_check(fd,60,110,-20000));
        CHECK(!cmd(fd,LR2021_TCP_SET_PPM,50));
        CHECK(wait_sim(lr2021_tcp_correct(TONE_HZ+20000,50),11,0,3));
        fprintf(stderr,"rejected frequency keeps streaming and the other settings\n");
        CHECK(!cmd(fd,LR2021_TCP_SET_PPM,0) && !cmd(fd,LR2021_TCP_SET_FREQ,50000000));
        CHECK(wait_sim(TONE_HZ+20000,11,0,3));
        CHECK(!stream_check(fd,60,110,-20000));
        fprintf(stderr,"tuner AGC and RTL AGC select the chip AGC\n");
        CHECK(!cmd(fd,LR2021_TCP_SET_FREQ,TONE_HZ-10000) && !cmd(fd,LR2021_TCP_SET_GAIN_MODE,0));
        CHECK(wait_sim(TONE_HZ-10000,0,0,3));
        CHECK(!cmd(fd,LR2021_TCP_SET_GAIN_MODE,1));
        CHECK(wait_sim(TONE_HZ-10000,11,0,3));
        CHECK(!cmd(fd,LR2021_TCP_SET_AGC,1));
        CHECK(wait_sim(TONE_HZ-10000,0,0,3));
        CHECK(!cmd(fd,LR2021_TCP_SET_RATE,125000));
        CHECK(wait_sim(TONE_HZ-10000,0,0,2));
        CHECK(!cmd(fd,LR2021_TCP_SET_RATE,250000));
        CHECK(wait_sim(TONE_HZ-10000,0,0,3));
        /* Disconnect mid-stream; the server must take the next client. */
        lrh_close(fd);
    }
    lr2021_tcp_stop(t);
    lrh_thread_join(st);
    lr2021_tcp_close(t);
    lrh_thread_join(bt);
    /* What the server told its user. */
    static char log[65536];
    rewind(cfg.log);
    size_t len=fread(log,1,sizeof(log)-1,cfg.log); log[len]=0;
    fclose(cfg.log);
    fputs(log,stderr);
    CHECK(strstr(log,"set the client to 250000") && strstr(log,"R820T index 15, LR2021 level 11"));
    CHECK(strstr(log,"50000000 Hz at 250000 samples/s rejected by the device"));
    CHECK(strstr(log,"refused: serving") && strstr(log,"stream ended by the device (reason 1"));
    CHECK(strstr(log,"stream restarts (length unknown)"));
    char *last=log, *p;
    while((p=strstr(last+1,"gone after")))last=p;
    CHECK(last!=log && strstr(last,"0 lost in 0 gaps") && !strstr(last," 0 stream restarts"));
    puts("rtl_tcp server over the bridge simulator passed");
    return 0;
}
