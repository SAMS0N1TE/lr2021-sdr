/* SPDX-License-Identifier: GPL-3.0-only */
#define _DEFAULT_SOURCE
#include "host_internal.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

struct lrh_server {
    int fd;
    lr2021_bridge_server srv;
    pthread_t reader, worker, sender;
    pthread_mutex_t send_mu, mu;
    pthread_cond_t cv;
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
static void lock_cb(void *c) { pthread_mutex_lock(&((lrh_server *)c)->send_mu); }
static void unlock_cb(void *c) { pthread_mutex_unlock(&((lrh_server *)c)->send_mu); }
static void wake_cb(void *c)
{
    lrh_server *s=c;
    pthread_mutex_lock(&s->mu); s->wake=true; pthread_cond_broadcast(&s->cv); pthread_mutex_unlock(&s->mu);
}
static void *reader(void *c)
{
    lrh_server *s=c;
    uint8_t buf[4096];
    while(!lr2021_bridge_server_exit_requested(&s->srv)) {
        ssize_t n=read(s->fd,buf,sizeof(buf));
        if(n<0 && errno==EINTR)continue;
        if(n<=0)break;
        lr2021_bridge_server_input(&s->srv,buf,(size_t)n);
    }
    lr2021_bridge_server_stop(&s->srv);
    pthread_mutex_lock(&s->mu); s->quit=true; s->reader_done=true;
    pthread_cond_broadcast(&s->cv); pthread_mutex_unlock(&s->mu);
    return NULL;
}
static void *worker(void *c)
{
    lrh_server *s=c;
    /* The FIFO service deadline is 2 ms at 125 ksample/s; ask for real-time
     * scheduling and carry on without it when not permitted. */
    struct sched_param sp={.sched_priority=50};
    pthread_setschedparam(pthread_self(),SCHED_FIFO,&sp);
    for(;;) {
        pthread_mutex_lock(&s->mu);
        while(!s->wake && !s->quit)pthread_cond_wait(&s->cv,&s->mu);
        bool quit=s->quit && !s->wake; s->wake=false;
        pthread_mutex_unlock(&s->mu);
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
        pthread_mutex_lock(&s->mu); bool quit=s->quit; pthread_mutex_unlock(&s->mu);
        if(quit && !atomic_load(&s->srv.streaming) && !atomic_load(&s->srv.busy) &&
           !lr2021_bridge_server_pump(&s->srv))break;
        lrh_sleep_us(200);
    }
    return NULL;
}
lrh_server *lrh_server_start(int fd, const lr2021_bridge_radio *radio, size_t tx_capacity)
{
    lrh_server *s=calloc(1,sizeof(*s));
    if(!s)return NULL;
    s->fd=fd;
    if(tx_capacity && !(s->tx_buf=malloc(tx_capacity))){ free(s); return NULL; }
    pthread_mutex_init(&s->send_mu,NULL); pthread_mutex_init(&s->mu,NULL); pthread_cond_init(&s->cv,NULL);
    lr2021_bridge_io io={s,send_cb,lock_cb,unlock_cb,wake_cb};
    if(lr2021_bridge_server_init(&s->srv,&io,radio,s->tx_buf,tx_capacity)){ free(s->tx_buf); free(s); return NULL; }
    pthread_create(&s->worker,NULL,worker,s);
    pthread_create(&s->sender,NULL,sender,s);
    pthread_create(&s->reader,NULL,reader,s);
    return s;
}
void lrh_server_wait(lrh_server *s)
{
    if(!s)return;
    pthread_join(s->reader,NULL);
    pthread_join(s->worker,NULL);
    pthread_join(s->sender,NULL);
}
void lrh_server_free(lrh_server *s)
{
    if(!s)return;
    pthread_mutex_destroy(&s->send_mu); pthread_mutex_destroy(&s->mu); pthread_cond_destroy(&s->cv);
    free(s->tx_buf); free(s);
}
