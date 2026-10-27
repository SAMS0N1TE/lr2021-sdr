/* SPDX-License-Identifier: GPL-3.0-only */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "host_internal.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sched.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <IOKit/serial/ioss.h>
#endif

uint64_t lrh_now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000000u+(uint64_t)t.tv_nsec/1000u;
}
void lrh_sleep_us(unsigned us)
{
    struct timespec t={us/1000000u,(long)(us%1000000u)*1000};
    while(nanosleep(&t,&t) && errno==EINTR){}
}
void lrh_mutex_init(lrh_mutex *m) { pthread_mutex_init(m,NULL); }
void lrh_mutex_destroy(lrh_mutex *m) { pthread_mutex_destroy(m); }
void lrh_lock(lrh_mutex *m) { pthread_mutex_lock(m); }
void lrh_unlock(lrh_mutex *m) { pthread_mutex_unlock(m); }
void lrh_cond_init(lrh_cond *c) { pthread_cond_init(c,NULL); }
void lrh_cond_destroy(lrh_cond *c) { pthread_cond_destroy(c); }
void lrh_cond_broadcast(lrh_cond *c) { pthread_cond_broadcast(c); }
void lrh_cond_wait(lrh_cond *c, lrh_mutex *m) { pthread_cond_wait(c,m); }
bool lrh_cond_wait_until(lrh_cond *c, lrh_mutex *m, uint64_t deadline_us)
{
    /* Condition variables wait on CLOCK_REALTIME (macOS has no other). */
    uint64_t now=lrh_now_us();
    if(now>=deadline_us)return false;
    uint64_t left=deadline_us-now;
    struct timespec t;
    clock_gettime(CLOCK_REALTIME,&t);
    t.tv_sec+=(time_t)(left/1000000u); t.tv_nsec+=(long)(left%1000000u)*1000L;
    if(t.tv_nsec>=1000000000L){ t.tv_sec++; t.tv_nsec-=1000000000L; }
    return pthread_cond_timedwait(c,m,&t)!=ETIMEDOUT;
}
int lrh_thread_start(lrh_thread *t, void *(*fn)(void *), void *arg)
{ return pthread_create(t,NULL,fn,arg)?-1:0; }
void lrh_thread_join(lrh_thread t) { pthread_join(t,NULL); }
void lrh_thread_realtime(void)
{
    struct sched_param sp={.sched_priority=50};
    pthread_setschedparam(pthread_self(),SCHED_FIFO,&sp);
}
int lrh_net_init(void) { return 0; }
long lrh_read(lrh_fd fd, void *p, size_t n)
{
    ssize_t r;
    do r=read(fd,p,n); while(r<0 && errno==EINTR);
    if(r<0)return errno==EAGAIN || errno==EWOULDBLOCK ? LRH_AGAIN : -1;
    return (long)r;
}
void lrh_shutdown(lrh_fd fd) { shutdown(fd,SHUT_RDWR); }
void lrh_close(lrh_fd fd) { close(fd); }
int lrh_socketpair(lrh_fd sv[2])
{
    if(socketpair(AF_UNIX,SOCK_STREAM,0,sv))return -1;
#ifdef SO_NOSIGPIPE
    int one=1;
    setsockopt(sv[0],SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
    setsockopt(sv[1],SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
#endif
    return 0;
}
int lrh_write_all(int fd, const uint8_t *p, size_t n)
{
    while(n) {
        ssize_t w=write(fd,p,n);
        if(w<0 && errno==EINTR)continue;
        if(w<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) {
            struct pollfd q={fd,POLLOUT,0};
            if(poll(&q,1,2000)<=0)return -1;
            continue;
        }
        if(w<=0)return -1;
        p+=w; n-=(size_t)w;
    }
    return 0;
}

#ifdef __linux__
int lrh_linux_baud(int fd, unsigned baud); /* host_linux_baud.c: termios2 */
#endif
int lrh_serial_baud(int fd, unsigned baud)
{
#ifdef __linux__
    return lrh_linux_baud(fd,baud);
#elif defined(__APPLE__)
    speed_t s=baud;
    return ioctl(fd,IOSSIOSPEED,&s);
#else
    struct termios t;
    if(tcgetattr(fd,&t))return -1;
    cfsetispeed(&t,baud); cfsetospeed(&t,baud);
    return tcsetattr(fd,TCSANOW,&t);
#endif
}
void lrh_serial_flush(int fd) { tcflush(fd,TCIOFLUSH); }
int lrh_serial_open(const char *path, unsigned baud, char *err, size_t n)
{
    int fd=open(path,O_RDWR|O_NOCTTY|O_NONBLOCK);
    if(fd<0){ snprintf(err,n,"open %s: %s",path,strerror(errno)); return -1; }
    /* Deassert DTR and RTS straight away: on boards with an auto-reset circuit
     * a held line keeps the ESP32 in reset or download mode. */
    int lines=TIOCM_DTR|TIOCM_RTS;
    ioctl(fd,TIOCMBIC,&lines);
    struct termios t;
    if(tcgetattr(fd,&t)){ snprintf(err,n,"%s is not a serial port",path); close(fd); return -1; }
    cfmakeraw(&t);
    t.c_cflag|=CLOCAL|CREAD; t.c_cflag&=~(tcflag_t)(HUPCL|CRTSCTS|CSTOPB|PARENB);
    t.c_cc[VMIN]=0; t.c_cc[VTIME]=1;
    cfsetispeed(&t,B115200); cfsetospeed(&t,B115200);
    if(tcsetattr(fd,TCSANOW,&t) || lrh_serial_baud(fd,baud)) {
        snprintf(err,n,"%s: cannot set %u baud",path,baud); close(fd); return -1;
    }
    ioctl(fd,TIOCMBIC,&lines);
    fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)&~O_NONBLOCK);
    return fd;
}
int lrh_tcp_connect(const char *host, unsigned port, unsigned timeout_ms, char *err, size_t n)
{
    char service[16]; snprintf(service,sizeof(service),"%u",port);
    struct addrinfo hints={0},*res=NULL,*a;
    hints.ai_socktype=SOCK_STREAM;
    int e=getaddrinfo(host,service,&hints,&res);
    if(e){ snprintf(err,n,"%s: %s",host,gai_strerror(e)); return -1; }
    int fd=-1;
    for(a=res;a;a=a->ai_next) {
        fd=socket(a->ai_family,a->ai_socktype,a->ai_protocol);
        if(fd<0)continue;
        fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)|O_NONBLOCK);
        if(!connect(fd,a->ai_addr,a->ai_addrlen) || errno==EINPROGRESS) {
            struct pollfd q={fd,POLLOUT,0}; int so=0; socklen_t len=sizeof(so);
            if(poll(&q,1,(int)timeout_ms)==1 && !getsockopt(fd,SOL_SOCKET,SO_ERROR,&so,&len) && !so)break;
        }
        close(fd); fd=-1;
    }
    freeaddrinfo(res);
    if(fd<0){ snprintf(err,n,"cannot connect to %s:%u",host,port); return -1; }
    fcntl(fd,F_SETFL,fcntl(fd,F_GETFL)&~O_NONBLOCK);
    int one=1; setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
#endif
    return fd;
}
int lrh_tcp_listen(const char *addr, unsigned port, char *err, size_t n)
{
    struct sockaddr_in a={0};
    a.sin_family=AF_INET; a.sin_port=htons((uint16_t)port); a.sin_addr.s_addr=htonl(INADDR_ANY);
    if(addr && *addr && inet_pton(AF_INET,addr,&a.sin_addr)!=1) {
        snprintf(err,n,"%s: not an IPv4 address",addr); return -1;
    }
    int fd=socket(AF_INET,SOCK_STREAM,0), one=1;
    if(fd<0){ snprintf(err,n,"socket: %s",strerror(errno)); return -1; }
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    if(bind(fd,(struct sockaddr *)&a,sizeof(a)) || listen(fd,1)) {
        snprintf(err,n,"port %u: %s",port,strerror(errno)); close(fd); return -1;
    }
    return fd;
}
unsigned lrh_tcp_port(lrh_fd fd)
{
    struct sockaddr_in a; socklen_t len=sizeof(a);
    if(getsockname(fd,(struct sockaddr *)&a,&len) || a.sin_family!=AF_INET)return 0;
    return ntohs(a.sin_port);
}
int lrh_poll_read(lrh_fd fd, unsigned timeout_ms)
{
    struct pollfd q={fd,POLLIN,0};
    int r;
    do r=poll(&q,1,timeout_ms==LRH_FOREVER?-1:(int)timeout_ms); while(r<0 && errno==EINTR);
    return r<0?-1:r?1:0;
}
lrh_fd lrh_tcp_accept(lrh_fd ls, unsigned timeout_ms, char *peer, size_t peer_size)
{
    if(lrh_poll_read(ls,timeout_ms)<=0)return -1;
    struct sockaddr_in a; socklen_t len=sizeof(a);
    int fd=accept(ls,(struct sockaddr *)&a,&len);
    if(fd<0)return -1;
    int one=1; setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(fd,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
#endif
    if(peer && peer_size) {
        char ip[INET_ADDRSTRLEN]="?";
        if(a.sin_family==AF_INET)inet_ntop(AF_INET,&a.sin_addr,ip,sizeof(ip));
        snprintf(peer,peer_size,"%s:%u",ip,(unsigned)ntohs(a.sin_port));
    }
    return fd;
}
