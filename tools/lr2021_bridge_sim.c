/* SPDX-License-Identifier: GPL-3.0-only */
/* Bridge protocol server on TCP backed by the LR2021 simulator, for host tests,
 * SoapySDR and Rust client development without hardware.
 *   lr2021_bridge_sim [--port 51021] [--pattern counter|tone] [--tone HZ] [--once]
 * Port 0 picks a free port. Prints "lr2021_bridge_sim: port N" when ready. */
#define _DEFAULT_SOURCE
#include "../src/host/host_internal.h"
#include "lr2021_sim.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#ifdef SIGPIPE
    signal(SIGPIPE,SIG_IGN);
#endif
    char err[160];
    lrh_fd ls=lrh_tcp_listen(NULL,port,err,sizeof(err));
    if(ls==LRH_NO_FD){ fprintf(stderr,"lr2021_bridge_sim: %s\n",err); return 1; }
    printf("lr2021_bridge_sim: port %u\n",lrh_tcp_port(ls)); fflush(stdout);
    do {
        lrh_fd fd=lrh_tcp_accept(ls,LRH_FOREVER,NULL,0);
        if(fd==LRH_NO_FD)continue;
        static lr2021_sim sim; static lr2021_iq_dev dev;
        lr2021_sim_init(&sim);
        sim.clock=clock_us; sim.sleep=sleep_cb; sim.pattern=pattern; sim.tone_hz=tone;
        sim.stall_tolerant=true;
        lr2021_iq_bus bus=lr2021_sim_bus(&sim);
        const lr2021_iq_rx rx={154785000,31250,13,0,LR2021_IQ_PATH_AUTO,0,0,0};
        if(lr2021_iq_dev_open(&dev,&bus,NULL,&rx)){ lrh_close(fd); continue; }
        lr2021_bridge_radio radio={&dev,&dev.hal,&dev.board,configure,rx,1,24,
            LR2021_BRIDGE_CAP_RX|LR2021_BRIDGE_CAP_TX|LR2021_BRIDGE_CAP_TIMED_TX|
            LR2021_BRIDGE_CAP_LIVE_GAIN|LR2021_BRIDGE_CAP_RATE_250K,"lr2021-sdr simulator","sim-tcp"};
        lrh_server *s=lrh_server_start(fd,&radio,LR2021_BRIDGE_TX_CAPACITY);
        if(s){ lrh_server_wait(s); lrh_server_free(s); }
        lrh_close(fd);
    } while(!once);
    lrh_close(ls);
    return 0;
}
