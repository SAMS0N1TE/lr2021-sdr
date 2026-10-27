/* SPDX-License-Identifier: MIT */
/* rtl_tcp-compatible server for the LR2021, so rtl_tcp clients (SDR++, SDR#,
 * gqrx, OpenWebRX, GNU Radio) can receive with it without SoapySDR.
 *   lr2021_tcp -d serial=COM5 [-a 127.0.0.1] [-p 1234] [-f 433.92M] ...
 * See docs/rtl-tcp.md; --help lists the options. Receive only. */
#include "lr2021_tcp.h"
#include <signal.h>
#include <stdio.h>

static lr2021_tcp *server;
static void on_signal(int s) { (void)s; if(server)lr2021_tcp_stop(server); }

int main(int argc, char **argv)
{
    lr2021_tcp_config cfg;
    char err[256];
    lr2021_tcp_config_init(&cfg);
    int r=lr2021_tcp_parse(&cfg,argc,argv,err,sizeof(err));
    if(r){
        if(r<0)fprintf(stderr,"lr2021_tcp: %s\n",err);
        lr2021_tcp_usage(r<0?stderr:stdout,argv[0]);
        return r<0?2:0;
    }
#ifdef SIGPIPE
    signal(SIGPIPE,SIG_IGN);
#endif
    if(!(server=lr2021_tcp_open(&cfg,err,sizeof(err)))) {
        fprintf(stderr,"lr2021_tcp: %s\n",err);
        return 1;
    }
    signal(SIGINT,on_signal);
    signal(SIGTERM,on_signal);
    r=lr2021_tcp_serve(server);
    lr2021_tcp_close(server);
    return r?1:0;
}
