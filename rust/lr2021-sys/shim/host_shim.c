/* SPDX-License-Identifier: GPL-3.0-only */
/* POSIX-only helpers: heap host arguments and a simulator bridge server on a
 * caller-supplied file descriptor (used by the Rust cross tests). */
#include "../../../src/host/host_internal.h"
#include "lr2021_sim.h"
#include <stdlib.h>

lr2021_host_args *lr2021_rs_args_new(void)
{ lr2021_host_args *a=malloc(sizeof(*a)); if(a)lr2021_host_args_init(a); return a; }
void lr2021_rs_args_free(lr2021_host_args *a) { free(a); }
size_t lr2021_rs_host_meta_size(void) { return sizeof(lr2021_host_rx_meta); }

static uint64_t clock_us(void *c) { (void)c; return lrh_now_us(); }
static void sleep_cb(void *c, unsigned us) { (void)c; lrh_sleep_us(us); }
static int configure(void *c, const lr2021_iq_rx *rx, uint16_t *errors)
{ lr2021_iq_dev *d=c; int e=lr2021_iq_dev_configure(d,rx); *errors=d->errors; return e; }
/* Serves one connection with a fresh real-time simulator; returns when the
 * peer disconnects or sends BYE. pattern 0 counter, 1 tone. */
int lr2021_rs_serve_sim(int fd, int pattern)
{
    lr2021_sim *sim=calloc(1,sizeof(*sim));
    lr2021_iq_dev *dev=calloc(1,sizeof(*dev));
    int e=sim && dev ? 0 : -1;
    if(!e) {
        lr2021_sim_init(sim);
        sim->clock=clock_us; sim->sleep=sleep_cb; sim->pattern=pattern; sim->stall_tolerant=true;
        lr2021_iq_bus bus=lr2021_sim_bus(sim);
        const lr2021_iq_rx rx={154785000,31250,13,0,LR2021_IQ_PATH_AUTO};
        e=lr2021_iq_dev_open(dev,&bus,NULL,&rx);
        if(!e) {
            lr2021_bridge_radio radio={dev,&dev->hal,NULL,configure,rx,1,24,
                LR2021_BRIDGE_CAP_RX|LR2021_BRIDGE_CAP_TX|LR2021_BRIDGE_CAP_TIMED_TX|
                LR2021_BRIDGE_CAP_LIVE_GAIN|LR2021_BRIDGE_CAP_RATE_250K,"lr2021-sdr simulator","rust-test"};
            lrh_server *s=lrh_server_start(fd,&radio,LR2021_BRIDGE_TX_CAPACITY);
            if(s){ lrh_server_wait(s); lrh_server_free(s); } else e=-1;
        }
    }
    free(dev); free(sim);
    return e;
}
