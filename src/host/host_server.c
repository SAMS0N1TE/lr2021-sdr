/* SPDX-License-Identifier: GPL-3.0-only */
#define _DEFAULT_SOURCE
#include "host_internal.h"
#include <errno.h>
#include <stdlib.h>
#ifndef _WIN32
#include <sys/socket.h>
#endif

struct lrh_server {
    lrh_fd fd;
    lr2021_bridge_server srv;
    lrh_thread reader, worker, sender;
    lrh_mutex send_mu, mu;
    lrh_cond cv;
    bool wake, quit, reader_done;
    uint8_t *tx_buf;
};
static int send_cb(void *c, const uint8_t *p, size_t n)
{
    lrh_server *s=c;
#ifdef MSG_NOSIGNAL
    while(n) {
        ssize_t w=send(s->fd,p,n,MSG_NOSIGNAL);
        if(w<0 && errno==ENOTSOCK)return lrh_write_all(s->fd,p,n);
        if(w<0 && errno==EINTR)continue;
        if(w<=0)return -1;
        p+=w; n-=(size_t)w;
    }
    return 0;
#else
    return lrh_write_all(s->fd,p,n);
#endif
}
static void lock_cb(void *c) { lrh_lock(&((lrh_server *)c)->send_mu); }
static void unlock_cb(void *c) { lrh_unlock(&((lrh_server *)c)->send_mu); }
static void wake_cb(void *c)
{
    lrh_server *s=c;
    lrh_lock(&s->mu); s->wake=true; lrh_cond_broadcast(&s->cv); lrh_unlock(&s->mu);
}
static void *reader(void *c)
{
    lrh_server *s=c;
    uint8_t buf[4096];
    while(!lr2021_bridge_server_exit_requested(&s->srv)) {
        long n=lrh_read(s->fd,buf,sizeof(buf));
        if(n==LRH_AGAIN)continue;
        if(n<=0)break;
        lr2021_bridge_server_input(&s->srv,buf,(size_t)n);
    }
    lr2021_bridge_server_stop(&s->srv);
    lrh_lock(&s->mu); s->quit=true; s->reader_done=true;
    lrh_cond_broadcast(&s->cv); lrh_unlock(&s->mu);
    return NULL;
}
static void *worker(void *c)
{
    lrh_server *s=c;
    /* The FIFO service deadline is 2 ms at 125 ksample/s; ask for real-time
     * scheduling and carry on without it when not permitted. */
    lrh_thread_realtime();
    for(;;) {
        lrh_lock(&s->mu);
        while(!s->wake && !s->quit)lrh_cond_wait(&s->cv,&s->mu);
        bool quit=s->quit && !s->wake; s->wake=false;
        lrh_unlock(&s->mu);
        if(quit)break;
        lr2021_bridge_server_work(&s->srv);
    }
    return NULL;
}
static void *sender(void *c)
{
    lrh_server *s=c;
    for(;;) {
        if(lr2021_bridge_server_pump(&s->srv))continue;
        lrh_lock(&s->mu); bool quit=s->quit; lrh_unlock(&s->mu);
        if(quit && !atomic_load(&s->srv.streaming) && !atomic_load(&s->srv.busy) &&
           !lr2021_bridge_server_pump(&s->srv))break;
        lrh_sleep_us(200);
    }
    return NULL;
}
lrh_server *lrh_server_start(lrh_fd fd, const lr2021_bridge_radio *radio, size_t tx_capacity)
{
    lrh_server *s=calloc(1,sizeof(*s));
    if(!s)return NULL;
    s->fd=fd;
#ifdef SO_NOSIGPIPE
    int one=1; setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one)); /* macOS has no MSG_NOSIGNAL */
#endif
    if(tx_capacity && !(s->tx_buf=malloc(tx_capacity))){ free(s); return NULL; }
    lrh_mutex_init(&s->send_mu); lrh_mutex_init(&s->mu); lrh_cond_init(&s->cv);
    lr2021_bridge_io io={s,send_cb,lock_cb,unlock_cb,wake_cb};
    if(lr2021_bridge_server_init(&s->srv,&io,radio,s->tx_buf,tx_capacity)){ free(s->tx_buf); free(s); return NULL; }
    lrh_thread_start(&s->worker,worker,s);
    lrh_thread_start(&s->sender,sender,s);
    lrh_thread_start(&s->reader,reader,s);
    return s;
}
void lrh_server_wait(lrh_server *s)
{
    if(!s)return;
    lrh_thread_join(s->reader);
    lrh_thread_join(s->worker);
    lrh_thread_join(s->sender);
}
int lrh_server_tune(lrh_server *s, unsigned max_read, unsigned check_bytes)
{ return s ? lr2021_bridge_server_tune(&s->srv,max_read,check_bytes) : -1; }
void lrh_server_free(lrh_server *s)
{
    if(!s)return;
    lrh_mutex_destroy(&s->send_mu); lrh_mutex_destroy(&s->mu); lrh_cond_destroy(&s->cv);
    free(s->tx_buf); free(s);
}
