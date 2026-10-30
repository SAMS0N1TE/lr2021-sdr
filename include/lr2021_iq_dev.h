/* SPDX-License-Identifier: MIT */
#ifndef LR2021_IQ_DEV_H
#define LR2021_IQ_DEV_H
#include "lr2021_iq.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Generic LR2021 adapter over a raw SPI bus: frame/BUSY/status handling, reset
 * and clock setup, LoRa receive configuration for source-2 I/Q, and restore.
 * It builds the lr2021_iq_hal used by the capture, stream and TX functions.
 * The Linux spidev backend, the ESP32-P4 firmware and the simulator use it.
 *
 * Measured limits (research/rx-range-and-gain): reception tracks 100 MHz to
 * 2740 MHz; IMG_CAL is still flagged near 100 MHz and PLL_LOCK above about
 * 2750 MHz. LF below 1.5 GHz and HF above is the automatic path choice. */
#define LR2021_IQ_MIN_HZ 100000000u
#define LR2021_IQ_MAX_HZ 2740000000u
#define LR2021_IQ_HF_HZ 1500000000u
#define LR2021_IQ_RFSW_MAX 6
#define LR2021_IQ_ERR_PLL_LOCK 0x0004u
#define LR2021_IQ_ERR_IMG_CAL 0x0080u
enum { LR2021_IQ_PATH_LF=0, LR2021_IQ_PATH_HF=1, LR2021_IQ_PATH_AUTO=2 };

/* modes: bit0 standby, bit1 RX LF, bit2 TX LF, bit3 RX HF, bit4 TX HF.
 * pull: 0 none, 1 down, 2 up, 3 auto (SetDioFunction / SetDioRfSwitchConfig). */
typedef struct { uint8_t dio, modes, pull; } lr2021_iq_rfsw;
typedef struct {
    uint8_t tcxo, tcxo_voltage, rfsw_count; /* voltage code 0..7 */
    uint32_t tcxo_ticks;                    /* startup delay, 32 MHz periods */
    lr2021_iq_rfsw rfsw[LR2021_IQ_RFSW_MAX];
} lr2021_iq_board;
/* route 0: LoRa source 2, IQ16 at 4 x bandwidth (BW 31250, 62500, 125000 or
 * 250000 Hz). Routes 1, 2, 3: sources 0, 1 and 6 from an FSK setup whose RX
 * filter code is input_code, two bytes per pair (IQ8 for sources 0 and 1,
 * gain-tagged IQ6 for source 6). Sources 0 and 6 run at rate_hz; source 1 at
 * lr2021_iq_input_rate(input_code). Byte 0 of the route command, the input
 * code, sets these rates; the modem's own filter setting does not. */
enum { LR2021_IQ_ROUTE_SOURCE2=0, LR2021_IQ_ROUTE_SOURCE0=1, LR2021_IQ_ROUTE_SOURCE1=2,
       LR2021_IQ_ROUTE_SOURCE6=3, LR2021_IQ_ROUTE_RAW0B=4 };
typedef struct {
    uint32_t frequency_hz, bandwidth_hz;
    uint8_t gain_step, boost, path;      /* step 0 AGC or 1..13, boost 0..7 */
    uint8_t route, input_code;
    uint32_t rate_hz;                    /* routes 1..3 */
} lr2021_iq_rx;

typedef struct {
    void *context;
    int (*transfer)(void *, uint8_t *, size_t); /* one CS frame, in place */
    int (*busy)(void *);                        /* 1 high, 0 low, <0 error */
    int (*reset)(void *, bool asserted);
    int (*fast)(void *, bool);                  /* optional bus ownership */
    void (*lock)(void *);                       /* optional, non-recursive */
    void (*unlock)(void *);
    uint64_t (*now_us)(void *);
    void (*sleep_us)(void *, unsigned);
} lr2021_iq_bus;
typedef struct {
    lr2021_iq_bus bus;
    lr2021_iq_board board;
    lr2021_iq_rx rx;           /* saved receive configuration */
    lr2021_iq_profile profile; /* reported by the snapshot hook */
    lr2021_iq_hal hal;
    uint16_t errors;           /* GetErrors after the last configuration */
    bool bus_owned;
} lr2021_iq_dev;

/* Nominal source-2 complex pairs/s = 4 x LoRa bandwidth for the four measured
 * rows of docs/protocol.md; 0 for anything else. */
unsigned lr2021_iq_rate(uint32_t bandwidth_hz);
uint32_t lr2021_iq_bandwidth(unsigned rate);
/* Converter input rate selected by an FSK RX filter code: 2.6 x the filter
 * bandwidth of datasheet Table 11-4, which the code packs as
 * 8 MHz x K[code >> 6] / (((code >> 3) & 7) + 1) / 2^(code & 7) with
 * K = 1, 13/14, 13/15, 13/18. Source 1 delivers pairs at this rate (measured:
 * code 33 at 799958, code 28 at 124795 pairs/s); rate_hz does not set it. */
unsigned lr2021_iq_input_rate(uint8_t code);
/* Pairs/s and bytes per pair of a receive configuration. */
unsigned lr2021_iq_rx_rate(const lr2021_iq_rx *);
unsigned lr2021_iq_rx_pair_bytes(const lr2021_iq_rx *);
unsigned lr2021_iq_path(uint32_t hz, unsigned path);
int lr2021_iq_rx_check(const lr2021_iq_rx *);
const char *lr2021_iq_strerror(int);

/* Command helpers on any HAL. setup runs after a hardware reset: version check
 * (ROM 1.24 only), standby, ClearErrors, TCXO, LDO, fallback and Calibrate.
 * configure applies a receive configuration and fails on any radio error
 * except IMG_CAL, which is reported in *errors. */
int lr2021_iq_setup(const lr2021_iq_hal *, const lr2021_iq_board *, unsigned *major, unsigned *minor);
int lr2021_iq_board_rfsw(const lr2021_iq_hal *, const lr2021_iq_board *);
int lr2021_iq_configure(const lr2021_iq_hal *, const lr2021_iq_rx *, uint16_t *errors);

/* Builds dev->hal, resets, sets up and configures rx. The device must stay at a
 * fixed address while the HAL is in use. Not thread-safe: hold the HAL lock. */
int lr2021_iq_dev_open(lr2021_iq_dev *, const lr2021_iq_bus *, const lr2021_iq_board *,
                       const lr2021_iq_rx *);
int lr2021_iq_dev_configure(lr2021_iq_dev *, const lr2021_iq_rx *);
#ifdef __cplusplus
}
#endif
#endif
