/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef LR2021_TCP_H
#define LR2021_TCP_H
#include "lr2021_host.h"
#include <stdio.h>
/* rtl_tcp-compatible server over the host client (docs/rtl-tcp.md). A client
 * receives the 12-byte dongle header ("RTL0", tuner type, gain count, both
 * u32 big endian), sends 5-byte commands (u8 command, u32 big-endian value)
 * and reads unsigned 8-bit interleaved I/Q at the device's rate. The LR2021's
 * 16-bit samples are converted on the host. One client at a time; settings
 * persist between clients. */
#define LR2021_TCP_PORT 1234u
#define LR2021_TCP_TUNER_R820T 5u
#define LR2021_TCP_GAINS 29u          /* the R820T gain table rtl_tcp clients use */
#define LR2021_TCP_LEVELS 20u         /* 1..13 step, 14..20 step 13 with boost 1..7 */
enum {
    LR2021_TCP_SET_FREQ=0x01, LR2021_TCP_SET_RATE=0x02, LR2021_TCP_SET_GAIN_MODE=0x03,
    LR2021_TCP_SET_GAIN=0x04, LR2021_TCP_SET_PPM=0x05, LR2021_TCP_SET_IF_GAIN=0x06,
    LR2021_TCP_SET_TEST_MODE=0x07, LR2021_TCP_SET_AGC=0x08, LR2021_TCP_SET_DIRECT=0x09,
    LR2021_TCP_SET_OFFSET=0x0A, LR2021_TCP_SET_RTL_XTAL=0x0B, LR2021_TCP_SET_TUNER_XTAL=0x0C,
    LR2021_TCP_SET_GAIN_INDEX=0x0D, LR2021_TCP_SET_BIAS_TEE=0x0E
};

typedef struct {
    lr2021_host_args device;
    char listen[64];                  /* IPv4 address; default 127.0.0.1 */
    unsigned port;                    /* default 1234; 0 picks a free port */
    uint32_t frequency_hz, rate_hz;   /* initial; rate snaps to 125000 or 250000 */
    unsigned level;                   /* initial gain: 0 chip AGC, 1..20 */
    unsigned path;                    /* 0 LF, 1 HF, 2 auto by frequency */
    int ppm;                          /* reference correction, until a client sets one */
    double scale;                     /* u8 = floor(sample * scale) + 128, clamped */
    bool quiet;                       /* only errors, drops and clipping on the log */
    FILE *log;                        /* default stderr */
} lr2021_tcp_config;
typedef struct lr2021_tcp lr2021_tcp;

void lr2021_tcp_config_init(lr2021_tcp_config *);
/* Command line (see usage). Returns 0, 1 when help was asked for, -1 on a bad
 * argument with the reason in err. */
int lr2021_tcp_parse(lr2021_tcp_config *, int argc, char **argv, char *err, size_t n);
void lr2021_tcp_usage(FILE *, const char *program);
/* Opens the device and the listening socket. */
lr2021_tcp *lr2021_tcp_open(const lr2021_tcp_config *, char *err, size_t n);
unsigned lr2021_tcp_port(const lr2021_tcp *);
/* Serves clients one at a time until lr2021_tcp_stop; returns 0, or -1 when
 * out of memory. A device that fails is closed and reopened for the next
 * client. */
int lr2021_tcp_serve(lr2021_tcp *);
void lr2021_tcp_stop(lr2021_tcp *);  /* any thread, or a signal handler */
void lr2021_tcp_close(lr2021_tcp *);

/* Mappings, exposed for tests and documentation. */
uint32_t lr2021_tcp_snap_rate(uint32_t requested, bool have_250k);
extern const int lr2021_tcp_r820t_gains[LR2021_TCP_GAINS]; /* tenths of a dB */
unsigned lr2021_tcp_gain_index(int tenth_db);     /* nearest R820T table entry */
unsigned lr2021_tcp_index_level(unsigned index);  /* 0..28 to 1..20 */
void lr2021_tcp_level_gain(unsigned level, uint8_t *step, uint8_t *boost);
uint32_t lr2021_tcp_correct(uint32_t frequency_hz, int ppm);
/* Converts pairs of int16 to 2*pairs bytes; returns the clipped value count. */
size_t lr2021_tcp_convert(const int16_t *iq, uint8_t *out, size_t pairs, double scale);
#endif
