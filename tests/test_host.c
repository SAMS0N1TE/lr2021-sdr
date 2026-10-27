/* SPDX-License-Identifier: GPL-3.0-only */
/* Host client against the simulator: in-process, over TCP and over a pty
 * standing in for the USB serial port. Real time, about 6 seconds. */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600
#include "../src/host/host_internal.h"
#include "lr2021_sim.h"
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)

static uint64_t clock_us(void *c) { (void)c; return lrh_now_us(); }
static void sleep_cb(void *c, unsigned us) { (void)c; lrh_sleep_us(us); }
static int configure(void *c, const lr2021_iq_rx *rx, uint16_t *errors)
{ lr2021_iq_dev *d=c; int e=lr2021_iq_dev_configure(d,rx); *errors=d->errors; return e; }
typedef struct { int fd; lr2021_sim sim; lr2021_iq_dev dev; } served;
static void *serve(void *c)
{
    served *s=c;
    lr2021_sim_init(&s->sim);
    s->sim.clock=clock_us; s->sim.sleep=sleep_cb; s->sim.pattern=LR2021_SIM_COUNTER;
    s->sim.stall_tolerant=true;
    lr2021_iq_bus bus=lr2021_sim_bus(&s->sim);
    const lr2021_iq_rx rx={154785000,31250,13,0,2};
    if(lr2021_iq_dev_open(&s->dev,&bus,NULL,&rx))return NULL;
    lr2021_bridge_radio radio={&s->dev,&s->dev.hal,NULL,configure,rx,1,24,3,"t","served"};
    lrh_server *srv=lrh_server_start(s->fd,&radio,LR2021_BRIDGE_TX_CAPACITY);
    if(srv){ lrh_server_wait(srv); lrh_server_free(srv); }
    return NULL;
}
static void *accept_serve(void *c)
{
    served *s=c;
    int ls=s->fd;
    s->fd=accept(ls,NULL,NULL); close(ls);
    if(s->fd<0)return NULL;
    serve(s); close(s->fd); return NULL;
}
static void *serve_fd(void *c) { return serve(c); }
/* Reads for ms milliseconds; counts pairs, gaps in the counter pattern and
 * reported discontinuities. */
static int stream(lr2021_host *h, unsigned ms, uint64_t *pairs, unsigned *gaps, unsigned *disc, bool counter)
{
    static int16_t iq[2*4096];
    uint64_t end=lrh_now_us()+ms*1000u; uint32_t last=0; bool have=false;
    while(lrh_now_us()<end) {
        lr2021_host_rx_meta m;
        long n=lr2021_host_rx_read(h,iq,4096,100000,&m);
        if(n<0)return (int)n;
        if(m.discontinuity)(*disc)++;
        for(long i=0;i<n;i++) {
            uint32_t v=(uint16_t)iq[2*i]|(uint32_t)(uint16_t)iq[2*i+1]<<16;
            /* A jump is only an error when the client did not report it. */
            if(counter && have && v!=last+1 && !(i==0 && m.discontinuity))(*gaps)++;
            last=v; have=true;
        }
        *pairs+=(uint64_t)n;
    }
    return 0;
}
static int exercise(lr2021_host *h, unsigned seconds_ms, bool counter)
{
    const lr2021_bridge_hello *hi=lr2021_host_hello(h);
    CHECK(hi->version==1 && hi->rom_major==1 && hi->rom_minor==24 && hi->tx_capacity==LR2021_BRIDGE_TX_CAPACITY);
    lr2021_bridge_rx rx={433920000,62500,13,0,2}; lr2021_bridge_rx_ack ack;
    CHECK(!lr2021_host_set_rx(h,&rx,&ack) && ack.rate_hz==250000);
    rx.frequency_hz=3000000000u; CHECK(lr2021_host_set_rx(h,&rx,&ack)==LR2021_IQ_ARGUMENT);
    rx.frequency_hz=433920000;
    CHECK(!lr2021_host_rx_start(h,0) && lr2021_host_rx_active(h));
    CHECK(lr2021_host_set_rx(h,&rx,&ack)==LR2021_BRIDGE_EBUSY);
    uint64_t pairs=0; unsigned gaps=0, disc=0;
    uint64_t t0=lrh_now_us();
    CHECK(!stream(h,seconds_ms/2,&pairs,&gaps,&disc,counter));
    CHECK(!lr2021_host_set_gain(h,12,7));
    CHECK(!stream(h,seconds_ms/2,&pairs,&gaps,&disc,counter));
    double rate=(double)pairs*1e6/(double)(lrh_now_us()-t0);
    lr2021_bridge_rx_end end;
    CHECK(!lr2021_host_rx_stop(h,&end) && !lr2021_host_rx_active(h));
    fprintf(stderr,"  %.0f pairs/s, %llu pairs, end pairs %llu, dropped %u, gain changes %u, error %d/%d flags %02x\n",rate,
        (unsigned long long)pairs,(unsigned long long)end.pairs,(unsigned)end.dropped,(unsigned)end.gain_changes,(int)end.capture_error,(int)end.recovery_error,end.fifo_flags);
    /* A loaded machine may drop pairs; each drop must be counted and flagged. */
    CHECK(!gaps && !end.capture_error && !end.recovery_error && end.gain_changes==1);
    CHECK(!disc || end.dropped);
    CHECK(rate>100000 && rate<260000);
    uint64_t t=0; CHECK(!lr2021_host_time(h,&t) && t>0);
    static int8_t iq[2*9600];
    for(unsigned i=0;i<9600;i++){ iq[2*i]=(int8_t)(i%60-30); iq[2*i+1]=(int8_t)(30-i%60); }
    lr2021_bridge_tx_begin p={0,0,19200,433920000,-18,1,0,0,0};
    lr2021_bridge_tx_end te;
    CHECK(!lr2021_host_tx(h,iq,9600,&p,&te) && te.samples==9600 && !te.untested && !(te.tx_flags&0x30));
    CHECK(te.elapsed_us>=490000 && te.elapsed_us<700000);
    p.start_time=t+700000;
    CHECK(!lr2021_host_tx(h,iq,960,&p,&te) && te.start_time>=p.start_time && te.start_time<p.start_time+5000);
    p.start_time=0; p.rate_hz=10000;
    CHECK(lr2021_host_tx(h,iq,960,&p,&te)==LR2021_BRIDGE_ERANGE);
    p.flags=LR2021_IQ_TX_UNCHECKED;
    CHECK(!lr2021_host_tx(h,iq,960,&p,&te) && te.untested==LR2021_IQ_TX_UNTESTED_RATE);
    /* Receive resumes after TX with a new generation. */
    pairs=0; CHECK(!lr2021_host_rx_start(h,300));
    CHECK(!stream(h,500,&pairs,&gaps,&disc,counter) && !gaps && pairs>20000);
    CHECK(!lr2021_host_rx_stop(h,&end) && end.reason==0 && !end.capture_error && end.pairs<=75100);
    return 0;
}
int main(void)
{
    signal(SIGPIPE,SIG_IGN);
    char err[200];
    lr2021_host_args a;
    lr2021_host_args_init(&a);
    CHECK(lr2021_host_args_set(&a,"sim","counter")==0 && lr2021_host_args_set(&a,"nope","1")==1);
    CHECK(lr2021_host_args_set(&a,"port","70000")==-1 && lr2021_host_args_set(&a,"rfsw","8:2")==0);
    lr2021_iq_board b;
    CHECK(!lrh_parse_board(&a,&b) && b.rfsw_count==1 && b.rfsw[0].dio==8 && b.rfsw[0].modes==2 && b.rfsw[0].pull==3);
    CHECK(!lr2021_host_args_set(&a,"rfsw","6:0,7:0:2,8:2,10:0") && !lrh_parse_board(&a,&b) && b.rfsw_count==4);
    CHECK(!lr2021_host_args_set(&a,"rfsw","4:0") && lrh_parse_board(&a,&b));
    CHECK(!lr2021_host_args_set(&a,"tcxo","3.3") && !lr2021_host_args_set(&a,"rfsw","") && !lrh_parse_board(&a,&b) && b.tcxo_voltage==7);
    lr2021_host *h=lr2021_host_open(&a,err,sizeof(err));
    if(!h)fprintf(stderr,"%s\n",err);
    CHECK(h && lr2021_host_backend(h)==LR2021_HOST_SIM);
    fprintf(stderr,"in-process simulator\n");
    CHECK(!exercise(h,2000,true));
    lr2021_host_close(h);

    /* CS12 over the in-process link. */
    CHECK(!lr2021_host_args_set(&a,"format","cs12"));
    CHECK((h=lr2021_host_open(&a,err,sizeof(err))));
    CHECK(!lr2021_host_rx_start(h,0));
    uint64_t pairs=0; unsigned gaps=0, disc=0;
    CHECK(!stream(h,300,&pairs,&gaps,&disc,false) && pairs>10000);
    lr2021_bridge_rx_end end;
    CHECK(!lr2021_host_rx_stop(h,&end) && !end.capture_error && (!disc || end.dropped));
    lr2021_host_close(h);

    /* TCP. */
    static served srv; pthread_t th;
    int ls=lrh_tcp_listen(0,err,sizeof(err));
    CHECK(ls>=0);
    struct sockaddr_in sa; socklen_t len=sizeof(sa);
    getsockname(ls,(struct sockaddr *)&sa,&len);
    srv.fd=ls;
    pthread_create(&th,NULL,accept_serve,&srv);
    lr2021_host_args_init(&a);
    snprintf(a.host,sizeof(a.host),"127.0.0.1"); a.port=ntohs(sa.sin_port);
    CHECK((h=lr2021_host_open(&a,err,sizeof(err))) && lr2021_host_backend(h)==LR2021_HOST_TCP);
    CHECK(!strcmp(lr2021_host_hello(h)->id,"served"));
    fprintf(stderr,"TCP\n");
    CHECK(!exercise(h,1000,true));
    lr2021_host_close(h);
    pthread_join(th,NULL);

#ifdef __APPLE__
    /* IOSSIOSPEED does not apply to ptys; the serial path is Linux-tested. */
    puts("host client over simulator and TCP passed (pty skipped on macOS)");
    return 0;
#endif
    /* A pty in place of the USB serial port; the client sets 3 Mbaud. */
    int master=posix_openpt(O_RDWR|O_NOCTTY);
    CHECK(master>=0 && !grantpt(master) && !unlockpt(master));
    lr2021_host_args_init(&a);
    snprintf(a.serial,sizeof(a.serial),"%s",ptsname(master)); a.boot_ms=0;
    srv.fd=master;
    pthread_create(&th,NULL,serve_fd,&srv);
    if(!(h=lr2021_host_open(&a,err,sizeof(err))))fprintf(stderr,"%s\n",err);
    CHECK(h && lr2021_host_backend(h)==LR2021_HOST_SERIAL);
    fprintf(stderr,"pty serial\n");
    CHECK(!exercise(h,1000,true));
    lr2021_host_close(h); /* sends BYE */
    pthread_join(th,NULL);
    close(master);
    puts("host client over simulator, TCP and pty passed");
    return 0;
}
