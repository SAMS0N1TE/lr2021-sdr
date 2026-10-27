/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef LR2021_HOST_INTERNAL_H
#define LR2021_HOST_INTERNAL_H
#include "lr2021_host.h"
#include "lr2021_bridge_server.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

/* Operating system layer: host_io.c (POSIX) or host_win32.c (Windows).
 * A descriptor is a POSIX fd, or on Windows a SOCKET or a serial port HANDLE
 * (told apart by GetFileType). */
#ifdef _WIN32
typedef intptr_t lrh_fd;
typedef SRWLOCK lrh_mutex;
typedef CONDITION_VARIABLE lrh_cond;
typedef HANDLE lrh_thread;
#else
typedef int lrh_fd;
typedef pthread_mutex_t lrh_mutex;
typedef pthread_cond_t lrh_cond;
typedef pthread_t lrh_thread;
#endif
#define LRH_NO_FD ((lrh_fd)-1)
#define LRH_AGAIN (-2L)

uint64_t lrh_now_us(void);
void lrh_sleep_us(unsigned us);
void lrh_mutex_init(lrh_mutex *);
void lrh_mutex_destroy(lrh_mutex *);
void lrh_lock(lrh_mutex *);
void lrh_unlock(lrh_mutex *);
void lrh_cond_init(lrh_cond *);
void lrh_cond_destroy(lrh_cond *);
void lrh_cond_broadcast(lrh_cond *);
void lrh_cond_wait(lrh_cond *, lrh_mutex *);
/* Waits until signalled or lrh_now_us() passes deadline_us; false on timeout.
 * Wake-ups can be spurious. */
bool lrh_cond_wait_until(lrh_cond *, lrh_mutex *, uint64_t deadline_us);
int lrh_thread_start(lrh_thread *, void *(*fn)(void *), void *);
void lrh_thread_join(lrh_thread);
void lrh_thread_realtime(void);      /* best effort; ignored when not permitted */

int lrh_net_init(void);              /* Winsock start-up; no-op elsewhere */
/* Bytes read, 0 at end of stream (sockets) or a serial read timeout,
 * LRH_AGAIN to retry, -1 on error. */
long lrh_read(lrh_fd, void *, size_t);
int lrh_write_all(lrh_fd, const uint8_t *, size_t);
void lrh_shutdown(lrh_fd);           /* sockets: wakes a blocked reader */
void lrh_close(lrh_fd);
int lrh_socketpair(lrh_fd sv[2]);
lrh_fd lrh_serial_open(const char *path, unsigned baud, char *err, size_t n);
int lrh_serial_baud(lrh_fd, unsigned baud);
void lrh_serial_flush(lrh_fd);
lrh_fd lrh_tcp_connect(const char *host, unsigned port, unsigned timeout_ms, char *err, size_t n);
/* addr NULL or "" listens on every IPv4 address; port 0 picks a free port. */
lrh_fd lrh_tcp_listen(const char *addr, unsigned port, char *err, size_t n);
unsigned lrh_tcp_port(lrh_fd);       /* local port of a socket, 0 on error */
/* Waits up to timeout_ms for a connection; LRH_NO_FD on timeout or error.
 * peer receives the remote address as text when not NULL. */
lrh_fd lrh_tcp_accept(lrh_fd listener, unsigned timeout_ms, char *peer, size_t peer_size);
/* 1 when fd has bytes to read (or is closed), 0 after timeout_ms, -1 on error.
 * LRH_FOREVER waits indefinitely, here and in lrh_tcp_accept. */
#define LRH_FOREVER 0xFFFFFFFFu
int lrh_poll_read(lrh_fd, unsigned timeout_ms);

/* Bridge server over a file descriptor with reader, worker and sender threads.
 * Owns nothing it is given; lrh_server_wait returns when the peer disconnects
 * or sends BYE. */
typedef struct lrh_server lrh_server;
lrh_server *lrh_server_start(lrh_fd fd, const lr2021_bridge_radio *, size_t tx_capacity);
void lrh_server_wait(lrh_server *);
void lrh_server_free(lrh_server *);
int lrh_server_tune(lrh_server *, unsigned max_read, unsigned check_bytes);

/* Linux spidev + libgpiod bus; NULL context on other systems. */
void *lrh_spidev_open(lr2021_iq_bus *, const lr2021_host_args *, char *err, size_t n);
void lrh_spidev_close(void *);
int lrh_parse_board(const lr2021_host_args *, lr2021_iq_board *);
#endif
