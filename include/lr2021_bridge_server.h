/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef LR2021_BRIDGE_SERVER_H
#define LR2021_BRIDGE_SERVER_H
/* Device side of the bridge protocol, portable C11 with <stdatomic.h>.
 * The platform supplies three execution contexts and a byte transport:
 *   reader: feeds received bytes to lr2021_bridge_server_input(). Quick
 *           requests (HELLO, GET_TIME, RX_STOP, live SET_GAIN, TX upload, BYE)
 *           are answered here.
 *   worker: waits for io.wake, then calls lr2021_bridge_server_work(). Radio
 *           requests (SET_RX, RX_START, TX_COMMIT, idle SET_GAIN) run here;
 *           RX_START blocks in the stream session until stopped. Give it the
 *           priority the FIFO service deadline needs.
 *   sender: calls lr2021_bridge_server_pump() until it returns false, then
 *           waits briefly. It sends RX_DATA, RX_END and TX_END in order.
 * io.send is called from all three under io.lock/io.unlock. A send failure is
 * reported but does not stop the radio; RX blocks that do not fit the output
 * queue are dropped and counted, which the host sees as a first_pair gap. */
#include "lr2021_bridge.h"
#include "lr2021_iq_tx.h"
#include <stdatomic.h>
#ifdef __cplusplus
#error "C only"
#endif
#ifndef LR2021_BRIDGE_SLOTS
#define LR2021_BRIDGE_SLOTS 32u   /* power of two */
#endif
typedef struct {
    void *context;
    int (*send)(void *, const uint8_t *, size_t); /* 0 on success */
    void (*lock)(void *);
    void (*unlock)(void *);
    void (*wake)(void *);
} lr2021_bridge_io;
typedef struct {
    void *context;
    const lr2021_iq_hal *hal;
    const lr2021_iq_board *board;  /* RF switch table for TX, may be NULL */
    int (*configure)(void *, const lr2021_iq_rx *, uint16_t *errors);
    lr2021_iq_rx rx;               /* configuration already applied */
    uint8_t rom_major, rom_minor;
    uint16_t caps;
    const char *name, *id;
} lr2021_bridge_radio;
typedef struct {
    uint8_t type;
    uint16_t sequence, length;
    uint8_t payload[LR2021_BRIDGE_RX_DATA_HEADER+LR2021_BRIDGE_RX_MAX_BYTES];
} lr2021_bridge_slot;
typedef struct {
    lr2021_bridge_io io;
    lr2021_bridge_radio radio;
    lr2021_bridge_parser parser;
    atomic_bool busy, streaming, stop, exit;
    atomic_uint gain_request;      /* 0 none, else 0x10000 | boost << 8 | step */
    atomic_uint live_boost;        /* boost of the running stream */
    uint8_t req_type; uint16_t req_seq, req_length;
    uint8_t req[LR2021_BRIDGE_MAX_PAYLOAD];
    atomic_uint head, tail;
    lr2021_bridge_slot slots[LR2021_BRIDGE_SLOTS];
    uint16_t seq_data, seq_rx_end, seq_tx_end;
    uint32_t generation, rate_hz;
    atomic_uint dropped;
    uint64_t start_time;
    uint8_t *tx_buf; size_t tx_capacity;
    lr2021_bridge_tx_begin tx;
    uint32_t tx_next;
    atomic_int tx_state;           /* 0 none, 1 uploading, 2 running */
    lr2021_bridge_slot *cur;       /* RX slot being filled by the worker */
    uint64_t cur_first; size_t cur_pairs; uint8_t format, pair_bytes;
    uint8_t staging[LR2021_BRIDGE_RX_MAX_BYTES];
    uint8_t reader_frame[64], worker_frame[96], sender_frame[LR2021_BRIDGE_MAX_FRAME];
    uint32_t send_errors;
    unsigned max_read, check_bytes;  /* session tuning, 0 = library default */
    uint8_t stream_buf[4096+1024];
} lr2021_bridge_server;
/* tx_buf may be NULL to disable TX (TX_BEGIN then answers EUNSUPPORTED). */
int lr2021_bridge_server_init(lr2021_bridge_server *, const lr2021_bridge_io *,
                              const lr2021_bridge_radio *, uint8_t *tx_buf, size_t tx_capacity);
void lr2021_bridge_server_input(lr2021_bridge_server *, const uint8_t *, size_t);
void lr2021_bridge_server_work(lr2021_bridge_server *);
bool lr2021_bridge_server_pump(lr2021_bridge_server *);
bool lr2021_bridge_server_exit_requested(lr2021_bridge_server *);
/* Ask a running stream to stop (for a transport disconnect). */
void lr2021_bridge_server_stop(lr2021_bridge_server *);
/* FIFO read size (multiple of 4, up to 1020) and flag-check interval
 * (multiple of 4, max_read..4096) for the receive sessions; 0 keeps the
 * library default (256 and 1024). Set before RX_START, from any context while
 * no stream runs. Adapters with fast large SPI frames use 1020 and 4096. */
int lr2021_bridge_server_tune(lr2021_bridge_server *, unsigned max_read, unsigned check_bytes);
#endif
