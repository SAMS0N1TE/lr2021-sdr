/* SPDX-License-Identifier: GPL-3.0-only */
/* Windows side of the host operating system layer (host_io.c is the POSIX
 * one): Win32 threads, slim reader/writer locks and condition variables,
 * Winsock, and COM ports through the Win32 communications API. */
#include "host_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif

uint64_t lrh_now_us(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if(!f.QuadPart)QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart/f.QuadPart)*1000000u+(uint64_t)(c.QuadPart%f.QuadPart)*1000000u/(uint64_t)f.QuadPart;
}
/* Sleep() rounds up to the scheduler tick (often 15.6 ms), far longer than
 * the radio loop and the sender expect. A high-resolution waitable timer
 * (Windows 10 1803 and later) sleeps to within about half a millisecond; on
 * older systems short waits are a yielding spin. */
void lrh_sleep_us(unsigned us)
{
    uint64_t end=lrh_now_us()+us;
    HANDLE t=CreateWaitableTimerExW(NULL,NULL,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_ALL_ACCESS);
    if(t) {
        LARGE_INTEGER due; due.QuadPart=-(LONGLONG)us*10;
        if(SetWaitableTimer(t,&due,0,NULL,NULL,FALSE))WaitForSingleObject(t,INFINITE);
        CloseHandle(t);
    } else if(us>=2000)Sleep(us/1000u-1u);
    while(lrh_now_us()<end)SwitchToThread();
}
void lrh_mutex_init(lrh_mutex *m) { InitializeSRWLock(m); }
void lrh_mutex_destroy(lrh_mutex *m) { (void)m; }
void lrh_lock(lrh_mutex *m) { AcquireSRWLockExclusive(m); }
void lrh_unlock(lrh_mutex *m) { ReleaseSRWLockExclusive(m); }
void lrh_cond_init(lrh_cond *c) { InitializeConditionVariable(c); }
void lrh_cond_destroy(lrh_cond *c) { (void)c; }
void lrh_cond_broadcast(lrh_cond *c) { WakeAllConditionVariable(c); }
void lrh_cond_wait(lrh_cond *c, lrh_mutex *m) { SleepConditionVariableSRW(c,m,INFINITE,0); }
bool lrh_cond_wait_until(lrh_cond *c, lrh_mutex *m, uint64_t deadline_us)
{
    uint64_t now=lrh_now_us();
    if(now>=deadline_us)return false;
    uint64_t ms=(deadline_us-now+999u)/1000u;
    if(ms>=INFINITE)ms=INFINITE-1;
    if(SleepConditionVariableSRW(c,m,(DWORD)ms,0))return true;
    return GetLastError()!=ERROR_TIMEOUT;
}
typedef struct { void *(*fn)(void *); void *arg; } start_args;
static DWORD WINAPI trampoline(LPVOID p)
{
    start_args a=*(start_args *)p;
    free(p);
    a.fn(a.arg);
    return 0;
}
int lrh_thread_start(lrh_thread *t, void *(*fn)(void *), void *arg)
{
    start_args *a=malloc(sizeof(*a));
    if(!a)return -1;
    a->fn=fn; a->arg=arg;
    *t=CreateThread(NULL,0,trampoline,a,0,NULL);
    if(!*t){ free(a); return -1; }
    return 0;
}
void lrh_thread_join(lrh_thread t) { WaitForSingleObject(t,INFINITE); CloseHandle(t); }
void lrh_thread_realtime(void) { SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_TIME_CRITICAL); }

static INIT_ONCE net_once=INIT_ONCE_STATIC_INIT;
static int net_status;
static BOOL CALLBACK net_start(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    WSADATA w;
    net_status=WSAStartup(MAKEWORD(2,2),&w)?-1:0;
    return TRUE;
}
int lrh_net_init(void)
{
    InitOnceExecuteOnce(&net_once,net_start,NULL,NULL);
    return net_status;
}
static bool is_serial(lrh_fd fd) { return GetFileType((HANDLE)fd)==FILE_TYPE_CHAR; }
long lrh_read(lrh_fd fd, void *p, size_t n)
{
    if(n>0x40000000u)n=0x40000000u;
    if(is_serial(fd)) {
        DWORD got=0;
        if(!ReadFile((HANDLE)fd,p,(DWORD)n,&got,NULL))return -1;
        return (long)got;
    }
    int r=recv((SOCKET)fd,p,(int)n,0);
    if(r==SOCKET_ERROR)return WSAGetLastError()==WSAEINTR?LRH_AGAIN:-1;
    return r;
}
int lrh_write_all(lrh_fd fd, const uint8_t *p, size_t n)
{
    bool serial=is_serial(fd);
    while(n) {
        int chunk=n>0x10000u?0x10000:(int)n, w;
        if(serial) {
            DWORD done=0;
            if(!WriteFile((HANDLE)fd,p,(DWORD)chunk,&done,NULL) || !done)return -1;
            w=(int)done;
        } else {
            w=send((SOCKET)fd,(const char *)p,chunk,0);
            if(w==SOCKET_ERROR && WSAGetLastError()==WSAEINTR)continue;
            if(w<=0)return -1;
        }
        p+=w; n-=(size_t)w;
    }
    return 0;
}
void lrh_shutdown(lrh_fd fd) { if(!is_serial(fd))shutdown((SOCKET)fd,SD_BOTH); }
void lrh_close(lrh_fd fd)
{
    if(is_serial(fd))CloseHandle((HANDLE)fd);
    else closesocket((SOCKET)fd);
}
static void nodelay(SOCKET s) { BOOL one=TRUE; setsockopt(s,IPPROTO_TCP,TCP_NODELAY,(const char *)&one,sizeof(one)); }
/* No AF_UNIX socketpair on Windows: a loopback TCP connection, accepted only
 * from the socket that made it. */
int lrh_socketpair(lrh_fd sv[2])
{
    if(lrh_net_init())return -1;
    SOCKET ls=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP), a=INVALID_SOCKET, b=INVALID_SOCKET;
    struct sockaddr_in addr={0}, mine, peer; int len=sizeof(addr);
    addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    BOOL excl=TRUE;
    if(ls==INVALID_SOCKET)return -1;
    setsockopt(ls,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(const char *)&excl,sizeof(excl));
    if(bind(ls,(struct sockaddr *)&addr,sizeof(addr)) || listen(ls,1) ||
       getsockname(ls,(struct sockaddr *)&addr,&len))goto fail;
    if((a=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP))==INVALID_SOCKET ||
       connect(a,(struct sockaddr *)&addr,sizeof(addr)))goto fail;
    len=sizeof(mine);
    if(getsockname(a,(struct sockaddr *)&mine,&len))goto fail;
    for(int tries=0;tries<8;tries++) {
        len=sizeof(peer);
        b=accept(ls,(struct sockaddr *)&peer,&len);
        if(b==INVALID_SOCKET)goto fail;
        if(peer.sin_port==mine.sin_port && peer.sin_addr.s_addr==mine.sin_addr.s_addr)break;
        closesocket(b); b=INVALID_SOCKET;
    }
    if(b==INVALID_SOCKET)goto fail;
    closesocket(ls);
    nodelay(a); nodelay(b);
    sv[0]=(lrh_fd)a; sv[1]=(lrh_fd)b;
    return 0;
fail:
    if(a!=INVALID_SOCKET)closesocket(a);
    closesocket(ls);
    return -1;
}
static int set_baud(HANDLE h, unsigned baud)
{
    DCB d; memset(&d,0,sizeof(d)); d.DCBlength=sizeof(d);
    if(!GetCommState(h,&d))return -1;
    d.BaudRate=baud; d.ByteSize=8; d.Parity=NOPARITY; d.StopBits=ONESTOPBIT;
    d.fBinary=TRUE; d.fParity=FALSE; d.fOutxCtsFlow=FALSE; d.fOutxDsrFlow=FALSE;
    d.fDtrControl=DTR_CONTROL_DISABLE; d.fRtsControl=RTS_CONTROL_DISABLE;
    d.fDsrSensitivity=FALSE; d.fOutX=FALSE; d.fInX=FALSE; d.fErrorChar=FALSE;
    d.fNull=FALSE; d.fAbortOnError=FALSE;
    return SetCommState(h,&d)?0:-1;
}
int lrh_serial_baud(lrh_fd fd, unsigned baud) { return set_baud((HANDLE)fd,baud); }
void lrh_serial_flush(lrh_fd fd) { PurgeComm((HANDLE)fd,PURGE_RXCLEAR|PURGE_TXCLEAR); }
/* path is COM5 or \\.\COM5. */
lrh_fd lrh_serial_open(const char *path, unsigned baud, char *err, size_t n)
{
    char name[300];
    snprintf(name,sizeof(name),"%s%s",strncmp(path,"\\\\.\\",4)?"\\\\.\\":"",path);
    HANDLE h=CreateFileA(name,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
    if(h==INVALID_HANDLE_VALUE){ snprintf(err,n,"open %s: Windows error %lu",path,GetLastError()); return LRH_NO_FD; }
    /* Deassert DTR and RTS straight away: on boards with an auto-reset circuit
     * a held line keeps the ESP32 in reset or download mode. */
    EscapeCommFunction(h,CLRDTR); EscapeCommFunction(h,CLRRTS);
    if(GetFileType(h)!=FILE_TYPE_CHAR || set_baud(h,baud)) {
        snprintf(err,n,"%s: cannot set %u baud",path,baud); CloseHandle(h); return LRH_NO_FD;
    }
    SetupComm(h,1u<<16,1u<<16);
    /* A read returns as soon as bytes are there, or after 100 ms with none,
     * like VTIME=1 on POSIX. */
    COMMTIMEOUTS t={MAXDWORD,MAXDWORD,100,0,2000};
    SetCommTimeouts(h,&t);
    EscapeCommFunction(h,CLRDTR); EscapeCommFunction(h,CLRRTS);
    return (lrh_fd)h;
}
lrh_fd lrh_tcp_connect(const char *host, unsigned port, unsigned timeout_ms, char *err, size_t n)
{
    if(lrh_net_init()){ snprintf(err,n,"Winsock start-up failed"); return LRH_NO_FD; }
    char service[16]; snprintf(service,sizeof(service),"%u",port);
    struct addrinfo hints, *res=NULL, *a;
    memset(&hints,0,sizeof(hints)); hints.ai_socktype=SOCK_STREAM;
    int e=getaddrinfo(host,service,&hints,&res);
    if(e){ snprintf(err,n,"%s: cannot resolve (error %d)",host,e); return LRH_NO_FD; }
    SOCKET s=INVALID_SOCKET;
    for(a=res;a;a=a->ai_next) {
        s=socket(a->ai_family,a->ai_socktype,a->ai_protocol);
        if(s==INVALID_SOCKET)continue;
        u_long on=1; ioctlsocket(s,FIONBIO,&on);
        if(!connect(s,a->ai_addr,(int)a->ai_addrlen) || WSAGetLastError()==WSAEWOULDBLOCK) {
            fd_set w, x; FD_ZERO(&w); FD_ZERO(&x); FD_SET(s,&w); FD_SET(s,&x);
            struct timeval tv={(long)(timeout_ms/1000u),(long)(timeout_ms%1000u)*1000L};
            int so=0, len=sizeof(so);
            if(select(0,NULL,&w,&x,&tv)==1 && FD_ISSET(s,&w) &&
               !getsockopt(s,SOL_SOCKET,SO_ERROR,(char *)&so,&len) && !so)break;
        }
        closesocket(s); s=INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if(s==INVALID_SOCKET){ snprintf(err,n,"cannot connect to %s:%u",host,port); return LRH_NO_FD; }
    u_long off=0; ioctlsocket(s,FIONBIO,&off);
    nodelay(s);
    return (lrh_fd)s;
}
lrh_fd lrh_tcp_listen(const char *addr, unsigned port, char *err, size_t n)
{
    if(lrh_net_init()){ snprintf(err,n,"Winsock start-up failed"); return LRH_NO_FD; }
    struct sockaddr_in a={0};
    a.sin_family=AF_INET; a.sin_port=htons((u_short)port); a.sin_addr.s_addr=htonl(INADDR_ANY);
    if(addr && *addr && inet_pton(AF_INET,addr,&a.sin_addr)!=1) {
        snprintf(err,n,"%s: not an IPv4 address",addr); return LRH_NO_FD;
    }
    SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(s==INVALID_SOCKET){ snprintf(err,n,"socket: error %d",WSAGetLastError()); return LRH_NO_FD; }
    /* SO_REUSEADDR on Windows lets another program take the port over. */
    BOOL excl=TRUE; setsockopt(s,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,(const char *)&excl,sizeof(excl));
    if(bind(s,(struct sockaddr *)&a,sizeof(a)) || listen(s,1)) {
        snprintf(err,n,"port %u: error %d",port,WSAGetLastError()); closesocket(s); return LRH_NO_FD;
    }
    return (lrh_fd)s;
}
unsigned lrh_tcp_port(lrh_fd fd)
{
    struct sockaddr_in a; int len=sizeof(a);
    if(getsockname((SOCKET)fd,(struct sockaddr *)&a,&len) || a.sin_family!=AF_INET)return 0;
    return ntohs(a.sin_port);
}
int lrh_poll_read(lrh_fd fd, unsigned timeout_ms)
{
    fd_set r; FD_ZERO(&r); FD_SET((SOCKET)fd,&r);
    struct timeval tv={(long)(timeout_ms/1000u),(long)(timeout_ms%1000u)*1000L};
    int e=select(0,&r,NULL,NULL,timeout_ms==LRH_FOREVER?NULL:&tv);
    return e==SOCKET_ERROR?-1:e?1:0;
}
lrh_fd lrh_tcp_accept(lrh_fd ls, unsigned timeout_ms, char *peer, size_t peer_size)
{
    if(lrh_poll_read(ls,timeout_ms)<=0)return LRH_NO_FD;
    struct sockaddr_in a; int len=sizeof(a);
    SOCKET s=accept((SOCKET)ls,(struct sockaddr *)&a,&len);
    if(s==INVALID_SOCKET)return LRH_NO_FD;
    nodelay(s);
    if(peer && peer_size) {
        char ip[INET_ADDRSTRLEN]="?";
        if(a.sin_family==AF_INET)inet_ntop(AF_INET,&a.sin_addr,ip,sizeof(ip));
        snprintf(peer,peer_size,"%s:%u",ip,(unsigned)ntohs(a.sin_port));
    }
    return (lrh_fd)s;
}
