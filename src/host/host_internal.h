/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef LR2021_HOST_INTERNAL_H
#define LR2021_HOST_INTERNAL_H
#include "lr2021_host.h"
#include "lr2021_bridge_server.h"

uint64_t lrh_now_us(void);
void lrh_sleep_us(unsigned us);
int lrh_write_all(int fd, const uint8_t *, size_t);
int lrh_serial_open(const char *path, unsigned baud, char *err, size_t n);
int lrh_serial_baud(int fd, unsigned baud);
void lrh_serial_flush(int fd);
int lrh_tcp_connect(const char *host, unsigned port, unsigned timeout_ms, char *err, size_t n);
int lrh_tcp_listen(unsigned port, char *err, size_t n);

/* Bridge server over a file descriptor with reader, worker and sender threads.
 * Owns nothing it is given; lrh_server_wait returns when the peer disconnects
 * or sends BYE. */
typedef struct lrh_server lrh_server;
lrh_server *lrh_server_start(int fd, const lr2021_bridge_radio *, size_t tx_capacity);
void lrh_server_wait(lrh_server *);
void lrh_server_free(lrh_server *);

/* Linux spidev + libgpiod bus; NULL context on other systems. */
void *lrh_spidev_open(lr2021_iq_bus *, const lr2021_host_args *, char *err, size_t n);
void lrh_spidev_close(void *);
int lrh_parse_board(const lr2021_host_args *, lr2021_iq_board *);
#endif
