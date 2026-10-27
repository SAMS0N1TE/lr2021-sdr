/* SPDX-License-Identifier: GPL-3.0-only */
/* Bridge protocol server on TCP backed by the LR2021 simulator, for host tests,
 * SoapySDR and Rust client development without hardware.
 *   lr2021_bridge_sim [--port 51021] [--pattern counter|tone] [--tone HZ] [--once]
 * Port 0 picks a free port. Prints "lr2021_bridge_sim: port N" when ready. */
#define _DEFAULT_SOURCE
#include "../src/host/host_internal.h"
#include "lr2021_sim.h"
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static uint64_t clock_us(void *c) { (void)c; return lrh_now_us(); }
static void sleep_cb(void *c, unsigned us) { (void)c; lrh_sleep_us(us); }
static int configure(void *c, const lr2021_iq_rx *rx, uint16_t *errors)
{ lr2021_iq_dev *d=c; int e=lr2021_iq_dev_configure(d,rx); *errors=d->errors; return e; }

int main(int argc, char **argv)
{
    unsigned port=LR2021_BRIDGE_TCP_PORT; int pattern=LR2021_SIM_TONE, once=0;
    uint32_t tone=154810000u;
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"--port") && i+1<argc)port=(unsigned)strtoul(argv[++i],NULL,0);
        else if(!strcmp(argv[i],"--tone") && i+1<argc)tone=(uint32_t)strtoul(argv[++i],NULL,0);
        else if(!strcmp(argv[i],"--pattern") && i+1<argc)pattern=strcmp(argv[++i],"counter")?LR2021_SIM_TONE:LR2021_SIM_COUNTER;
        else if(!strcmp(argv[i],"--once"))once=1;
        else { fprintf(stderr,"usage: %s [--port N] [--pattern counter|tone] [--tone HZ] [--once]\n",argv[0]); return 2; }
    }
    signal(SIGPIPE,SIG_IGN);
    char err[160];
    int ls=lrh_tcp_listen(port,err,sizeof(err));
    if(ls<0){ fprintf(stderr,"lr2021_bridge_sim: %s\n",err); return 1; }
    struct sockaddr_in a; socklen_t len=sizeof(a);
    getsockname(ls,(struct sockaddr *)&a,&len);
    printf("lr2021_bridge_sim: port %u\n",(unsigned)ntohs(a.sin_port)); fflush(stdout);
    do {
        int fd=accept(ls,NULL,NULL);
        if(fd<0)continue;
        static lr2021_sim sim; static lr2021_iq_dev dev;
        lr2021_sim_init(&sim);
        sim.clock=clock_us; sim.sleep=sleep_cb; sim.pattern=pattern; sim.tone_hz=tone;
        sim.stall_tolerant=true;
        lr2021_iq_bus bus=lr2021_sim_bus(&sim);
        const lr2021_iq_rx rx={154785000,31250,13,0,LR2021_IQ_PATH_AUTO,0,0,0};
        if(lr2021_iq_dev_open(&dev,&bus,NULL,&rx)){ close(fd); continue; }
        lr2021_bridge_radio radio={&dev,&dev.hal,&dev.board,configure,rx,1,24,
            LR2021_BRIDGE_CAP_RX|LR2021_BRIDGE_CAP_TX|LR2021_BRIDGE_CAP_TIMED_TX|
            LR2021_BRIDGE_CAP_LIVE_GAIN|LR2021_BRIDGE_CAP_RATE_250K,"lr2021-sdr simulator","sim-tcp"};
        lrh_server *s=lrh_server_start(fd,&radio,LR2021_BRIDGE_TX_CAPACITY);
        if(s){ lrh_server_wait(s); lrh_server_free(s); }
        close(fd);
    } while(!once);
    close(ls);
    return 0;
}
