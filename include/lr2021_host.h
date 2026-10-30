/* SPDX-License-Identifier: MIT */
#ifndef LR2021_HOST_H
#define LR2021_HOST_H
#include "lr2021_bridge.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Host-side device for Linux, macOS and Windows (MinGW-w64). Every backend
 * speaks the bridge protocol (docs/bridge-protocol.md):
 *   serial: a bridge firmware on a USB serial port (/dev/ttyACM0, COM5)
 *   tcp:    a bridge firmware over Wi-Fi, or tools/lr2021_bridge_sim
 *   local:  the chip on this host's spidev and GPIO lines (Linux); the bridge
 *           server runs in-process over a socket pair
 *   sim:    the simulator in-process, for tests without hardware
 * Functions are thread-safe; one reader of rx_read at a time. */
enum { LR2021_HOST_SERIAL=1, LR2021_HOST_TCP=2, LR2021_HOST_LOCAL=3, LR2021_HOST_SIM=4 };
/* Host-side status codes, beyond the bridge codes. */
enum { LR2021_HOST_ETIMEOUT=-40, LR2021_HOST_ELINK=-41, LR2021_HOST_EPROTO=-42, LR2021_HOST_EARG=-43 };
#define LR2021_HOST_DEFAULT_BAUD 3000000u

typedef struct {
    int backend;                /* 0: chosen from the fields set below */
    char serial[256];           /* serial=/dev/ttyACM0 */
    unsigned baud, boot_ms;     /* bridge baud; wait after opening the port */
    char host[256];             /* host=192.168.4.1 */
    unsigned port;              /* port=51021 */
    /* local backend (Linux) */
    char spidev[128];           /* spidev=/dev/spidev0.0 */
    char gpiochip[128];         /* gpiochip=gpiochip0 or /dev/gpiochip0 */
    int reset_line, busy_line;  /* line offsets on gpiochip */
    unsigned spi_hz;            /* default 16000000 */
    char tcxo[8];               /* off, 1.6 .. 3.3 */
    unsigned tcxo_ticks;
    char rfsw[96];              /* dio:modes[:pull],... see lr2021_iq_rfsw */
    /* simulator */
    int sim_pattern;            /* 0 counter, 1 tone */
    uint32_t sim_tone_hz;
    unsigned timeout_ms;        /* request timeout, default 2000 */
    unsigned format;            /* RX_DATA format: LR2021_BRIDGE_CS16 or CS12 */
} lr2021_host_args;
typedef struct {
    uint64_t time_ns;           /* device time of the first returned pair (nominal) */
    uint64_t first_pair;
    uint32_t generation, rate_hz;
    bool discontinuity;         /* pairs were lost before this span */
    bool ended;                 /* stream ended; see lr2021_host_rx_end */
} lr2021_host_rx_meta;
typedef struct lr2021_host lr2021_host;

void lr2021_host_args_init(lr2021_host_args *);
/* key=value setter shared by SoapySDR kwargs and the Rust crate. Keys:
 * serial, baud, boot_ms, host, port, spidev, gpiochip, reset, busy, spi_hz,
 * tcxo, tcxo_ticks, rfsw, sim (counter|tone), tone, timeout_ms, format
 * (cs16|cs12). Unknown keys
 * return 1 (ignored), bad values -1. */
int lr2021_host_args_set(lr2021_host_args *, const char *key, const char *value);
lr2021_host *lr2021_host_open(const lr2021_host_args *, char *error, size_t error_size);
void lr2021_host_close(lr2021_host *);
int lr2021_host_backend(const lr2021_host *);
const lr2021_bridge_hello *lr2021_host_hello(const lr2021_host *);

int lr2021_host_set_rx(lr2021_host *, const lr2021_bridge_rx *, lr2021_bridge_rx_ack *);
int lr2021_host_set_gain(lr2021_host *, unsigned step, unsigned boost);
int lr2021_host_rx_start(lr2021_host *, unsigned duration_ms);
/* Copies up to max_pairs interleaved int16 I,Q pairs. Waits up to timeout_us
 * for data. Returns the pair count, 0 on timeout, or a negative error. A span
 * never crosses a discontinuity: meta describes exactly the returned pairs. */
long lr2021_host_rx_read(lr2021_host *, int16_t *iq, size_t max_pairs, unsigned timeout_us,
                         lr2021_host_rx_meta *);
/* Stops the stream and waits for RX_END (end may be NULL). */
int lr2021_host_rx_stop(lr2021_host *, lr2021_bridge_rx_end *end);
bool lr2021_host_rx_active(lr2021_host *);
/* Uploads and transmits one burst. p supplies rate, frequency, power, repeats
 * and start_time; bytes and crc32 are filled in. Blocks until TX_END. */
int lr2021_host_tx(lr2021_host *, const int8_t *iq, size_t pairs, const lr2021_bridge_tx_begin *p,
                   lr2021_bridge_tx_end *end);
int lr2021_host_time(lr2021_host *, uint64_t *device_us);
int lr2021_host_get_gain(lr2021_host *, lr2021_bridge_gain *);
const char *lr2021_host_strerror(int);
/* Probe-free listing of likely bridge serial ports (USB VID:PID 1A86:55D3
 * CH343, 303A:1001 ESP USB Serial/JTAG, 303A:4001 TinyUSB). Writes up to
 * max paths, returns the count. Ports are not opened. Returns 0 on Windows. */
size_t lr2021_host_list_serial(char paths[][256], size_t max);
#ifdef __cplusplus
}
#endif
#endif
