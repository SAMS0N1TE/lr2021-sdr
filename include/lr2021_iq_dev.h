/* SPDX-License-Identifier: GPL-3.0-only */
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
typedef struct {
    uint32_t frequency_hz, bandwidth_hz; /* LoRa BW 31250 or 62500 */
    uint8_t gain_step, boost, path;      /* step 0 AGC or 1..13, boost 0..7 */
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

/* Nominal complex pairs/s = 4 x LoRa bandwidth for the two streamable rows of
 * docs/protocol.md; 0 for anything else. */
unsigned lr2021_iq_rate(uint32_t bandwidth_hz);
uint32_t lr2021_iq_bandwidth(unsigned rate);
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
