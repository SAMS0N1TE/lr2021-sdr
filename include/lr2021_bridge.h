/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef LR2021_BRIDGE_H
#define LR2021_BRIDGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* LR2021-SDR bridge protocol, version 1: framed binary messages between a
 * host and a board that owns the radio, over any reliable or unreliable byte
 * stream (USB serial, UART, TCP). docs/bridge-protocol.md is the normative
 * description; this header is its C codec. Portable C11, no allocation.
 *
 * Frame, all integers little endian:
 *   0  'L' 0x4C          sync
 *   1  'R' 0x52
 *   2  u8  version       1
 *   3  u8  type
 *   4  u16 sequence      requests: chosen by the host; responses echo it;
 *                        device-originated messages count per message type
 *   6  u16 length        payload bytes, 0..LR2021_BRIDGE_MAX_PAYLOAD
 *   8  payload
 *   8+length u32 CRC-32  IEEE (reflected 0xEDB88320, init/final 0xFFFFFFFF)
 *                        over bytes 2..8+length-1
 * A receiver scans for the sync, checks version and length, then the CRC.
 * On any failure it discards one byte and rescans. */
#define LR2021_BRIDGE_VERSION 1u
#define LR2021_BRIDGE_HEADER 8u
#define LR2021_BRIDGE_MAX_PAYLOAD 2048u
#define LR2021_BRIDGE_MAX_FRAME (LR2021_BRIDGE_HEADER+LR2021_BRIDGE_MAX_PAYLOAD+4u)
#define LR2021_BRIDGE_RX_DATA_HEADER 32u
#define LR2021_BRIDGE_RX_MAX_PAIRS 480u
#define LR2021_BRIDGE_RX_MAX_BYTES (4u*LR2021_BRIDGE_RX_MAX_PAIRS)
#define LR2021_BRIDGE_TX_CHUNK 1024u
#define LR2021_BRIDGE_TX_CAPACITY 65534u
#define LR2021_BRIDGE_TCP_PORT 51021u

/* Requests (host to device). The response type is request | 0x80; its payload
 * starts with an i32 status (0 or a negative code below). */
enum {
    LR2021_BRIDGE_HELLO=0x01,    /* u16 host version */
    LR2021_BRIDGE_SET_RX=0x02,   /* lr2021_bridge_rx */
    LR2021_BRIDGE_RX_START=0x03, /* u16 fifo capacity, u32 duration ms (0 = until stop),
                                  * optional u8 sample format */
    LR2021_BRIDGE_RX_STOP=0x04,  /* empty */
    LR2021_BRIDGE_SET_GAIN=0x05, /* u8 step (0 AGC, 1..13), u8 boost (0..7) */
    LR2021_BRIDGE_TX_BEGIN=0x06, /* lr2021_bridge_tx_begin */
    LR2021_BRIDGE_TX_DATA=0x07,  /* u32 offset, data (even, 2..1024 bytes) */
    LR2021_BRIDGE_TX_COMMIT=0x08,/* empty; the burst result follows as TX_END */
    LR2021_BRIDGE_GET_TIME=0x09, /* empty */
    LR2021_BRIDGE_BYE=0x0A,      /* empty; device leaves bridge mode after replying */
    LR2021_BRIDGE_RESPONSE=0x80,
    /* Device-originated messages. */
    LR2021_BRIDGE_RX_DATA=0xC0,  /* lr2021_bridge_rx_data header + IQ16 LE pairs */
    LR2021_BRIDGE_RX_END=0xC1,   /* lr2021_bridge_rx_end */
    LR2021_BRIDGE_TX_END=0xC2    /* lr2021_bridge_tx_end */
};
/* Status codes. -1..-5 are the lr2021_iq codes (argument, state, timeout,
 * data, I/O) passed through from the radio library. */
enum {
    LR2021_BRIDGE_OK=0,
    LR2021_BRIDGE_EBUSY=-16,     /* worker busy, or command not allowed while streaming */
    LR2021_BRIDGE_EUNKNOWN=-17,  /* unknown request type */
    LR2021_BRIDGE_ELENGTH=-18,   /* payload length wrong for the type */
    LR2021_BRIDGE_ESTATE=-19,    /* wrong protocol state (no upload, not streaming...) */
    LR2021_BRIDGE_EUNSUPPORTED=-20,
    LR2021_BRIDGE_ECRC=-21,      /* TX upload CRC mismatch */
    LR2021_BRIDGE_ERANGE=-22     /* TX samples or limits outside the contract */
};
/* HELLO capability bits. */
enum {
    LR2021_BRIDGE_CAP_RX=1u, LR2021_BRIDGE_CAP_TX=2u, LR2021_BRIDGE_CAP_TIMED_TX=4u,
    LR2021_BRIDGE_CAP_LIVE_GAIN=8u, LR2021_BRIDGE_CAP_RATE_250K=16u,
    LR2021_BRIDGE_CAP_CS12=32u, LR2021_BRIDGE_CAP_TX_UNCHECKED=64u
};
/* RX_DATA sample formats. Both carry the radio's source-2 int16 pairs.
 * CS16: 4 bytes per pair, I then Q, int16 little endian.
 * CS12: 3 bytes per pair, block floating point for slow links: every value of
 * the frame is arithmetically shifted right by the header's shift (0..4, the
 * smallest that fits -2048..2047), then the 24-bit word I12 | Q12 << 12 is
 * stored little endian. A host multiplies by 2^shift. Exact when shift is 0. */
enum { LR2021_BRIDGE_CS16=0, LR2021_BRIDGE_CS12=1 };
/* RX_END reasons. */
enum {
    LR2021_BRIDGE_END_REQUEST=0,  /* RX_STOP or duration reached */
    LR2021_BRIDGE_END_ERROR=1,    /* radio, FIFO loss or recovery error */
    LR2021_BRIDGE_END_TX=2        /* reserved */
};

typedef struct {
    int32_t status;
    uint16_t version, caps;
    uint8_t rom_major, rom_minor;
    uint16_t max_payload;
    uint32_t clock_hz;          /* device time unit; 1000000 = microseconds */
    uint32_t tx_capacity;       /* TX upload bytes */
    char name[32], id[32];      /* NUL-padded firmware name/version, board id */
} lr2021_bridge_hello;          /* 84-byte response payload */
typedef struct {
    uint32_t frequency_hz, bandwidth_hz; /* LoRa BW 31250 or 62500 */
    uint8_t gain_step, boost, path;      /* path 0 LF, 1 HF, 2 auto */
} lr2021_bridge_rx;                      /* 11 bytes */
typedef struct {
    int32_t status;
    uint16_t radio_errors;      /* GetErrors after configuration */
    uint32_t rate_hz;           /* nominal complex pairs/s */
} lr2021_bridge_rx_ack;         /* 10 bytes */
typedef struct {
    uint32_t bytes, crc32, rate_hz, frequency_hz;
    int8_t power_half_db;       /* SetTxParams units */
    uint8_t repeats;
    uint64_t start_time;        /* device time units, 0 = immediately */
    uint8_t pa;                 /* 0 LF, 1 HF, 2 by frequency */
    uint8_t flags;              /* bit 0: unchecked, hardware limits only; bit 1: SIMO for the burst */
} lr2021_bridge_tx_begin;       /* 28 bytes */
typedef struct {
    uint32_t generation;        /* increments on every RX_START */
    uint32_t rate_hz;
    uint64_t first_pair;        /* index of the first pair since stream start */
    uint64_t start_time;        /* device time of pair 0 (nominal) */
    uint32_t dropped;           /* pairs dropped by the device queue so far */
    uint16_t pairs;             /* 1..480 */
    uint8_t format, shift;
} lr2021_bridge_rx_data;        /* 32 bytes, then the packed pairs */
typedef struct {
    uint32_t generation;
    int32_t capture_error, recovery_error;
    uint8_t reason, fifo_flags;
    uint16_t max_fifo_level;
    uint32_t dropped, gain_changes;
    uint64_t pairs, start_time, elapsed_us, max_service_gap_us;
} lr2021_bridge_rx_end;         /* 56 bytes */
typedef struct {
    int32_t error, stop_error, recovery_error;
    uint8_t tx_flags;
    uint16_t final_level, min_level;
    uint32_t irq, samples;
    uint64_t start_time, elapsed_us, max_service_gap_us;
    uint32_t untested;          /* LR2021_IQ_TX_UNTESTED_* bits */
} lr2021_bridge_tx_end;         /* 53 bytes */

typedef struct {
    uint8_t type;
    uint16_t sequence, length;
    const uint8_t *payload;
} lr2021_bridge_frame;
typedef void (*lr2021_bridge_frame_cb)(void *, const lr2021_bridge_frame *);
typedef struct {
    uint8_t buf[LR2021_BRIDGE_MAX_FRAME];
    size_t used;
    uint32_t frames, crc_errors, skipped_bytes;
} lr2021_bridge_parser;

uint32_t lr2021_bridge_crc32(uint32_t crc, const uint8_t *, size_t); /* start with 0 */
/* Returns the frame size, or 0 if the payload is too long or out is too small. */
size_t lr2021_bridge_encode(uint8_t *out, size_t capacity, uint8_t type, uint16_t sequence,
                           const uint8_t *payload, size_t length);
void lr2021_bridge_parser_init(lr2021_bridge_parser *);
/* Feed any number of bytes; callback runs for each valid frame, with the
 * payload pointer valid only during the call. */
void lr2021_bridge_parse(lr2021_bridge_parser *, const uint8_t *, size_t,
                         lr2021_bridge_frame_cb, void *);

/* Message codecs. pack returns the payload length; unpack returns 0 or -1 on a
 * wrong length. They check only sizes, not value ranges. */
size_t lr2021_bridge_pack_hello(uint8_t *, const lr2021_bridge_hello *);
int lr2021_bridge_unpack_hello(const uint8_t *, size_t, lr2021_bridge_hello *);
size_t lr2021_bridge_pack_rx(uint8_t *, const lr2021_bridge_rx *);
int lr2021_bridge_unpack_rx(const uint8_t *, size_t, lr2021_bridge_rx *);
size_t lr2021_bridge_pack_rx_ack(uint8_t *, const lr2021_bridge_rx_ack *);
int lr2021_bridge_unpack_rx_ack(const uint8_t *, size_t, lr2021_bridge_rx_ack *);
size_t lr2021_bridge_pack_tx_begin(uint8_t *, const lr2021_bridge_tx_begin *);
int lr2021_bridge_unpack_tx_begin(const uint8_t *, size_t, lr2021_bridge_tx_begin *);
size_t lr2021_bridge_pack_rx_data(uint8_t *, const lr2021_bridge_rx_data *);
int lr2021_bridge_unpack_rx_data(const uint8_t *, size_t, lr2021_bridge_rx_data *);
size_t lr2021_bridge_pack_rx_end(uint8_t *, const lr2021_bridge_rx_end *);
int lr2021_bridge_unpack_rx_end(const uint8_t *, size_t, lr2021_bridge_rx_end *);
size_t lr2021_bridge_pack_tx_end(uint8_t *, const lr2021_bridge_tx_end *);
int lr2021_bridge_unpack_tx_end(const uint8_t *, size_t, lr2021_bridge_tx_end *);

/* Sample packing for RX_DATA. pack returns the byte count and the shift used;
 * unpack writes 2*pairs int16 values and returns 0, or -1 on a bad format,
 * shift or length. */
size_t lr2021_bridge_sample_bytes(unsigned format, size_t pairs);
size_t lr2021_bridge_pack_samples(uint8_t *out, unsigned format, const uint8_t *cs16, size_t pairs,
                                  uint8_t *shift);
int lr2021_bridge_unpack_samples(int16_t *out, unsigned format, unsigned shift,
                                 const uint8_t *data, size_t bytes, size_t pairs);
void lr2021_bridge_put16(uint8_t *, uint16_t);
void lr2021_bridge_put32(uint8_t *, uint32_t);
void lr2021_bridge_put64(uint8_t *, uint64_t);
uint16_t lr2021_bridge_get16(const uint8_t *);
uint32_t lr2021_bridge_get32(const uint8_t *);
uint64_t lr2021_bridge_get64(const uint8_t *);
const char *lr2021_bridge_strerror(int);
#ifdef __cplusplus
}
#endif
#endif
