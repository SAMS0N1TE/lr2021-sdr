/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef LR2021_IQ_TX_H
#define LR2021_IQ_TX_H
#include "lr2021_iq_dev.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Finite I/Q burst through ROM 1.24 command 0x0293, selector 0, after OOK
 * setup (docs/tx-discovery.md, docs/tx-playback.md). Signed interleaved I8,Q8.
 * Limits are this repository's tested contract:
 *   rates 4800, 9600, 19200, 38400, 76800, 125000 complex samples/s;
 *   at most 32767 complex samples per run including repetitions (the ROM
 *   stores a 16-bit byte count for 2 x N); at most 900 ms of samples;
 *   1..16 repetitions; every sample I*I+Q*Q <= 56*56 (saturation starts near
 *   radius 64).
 * The burst holds the radio, uses the 1 KiB TX FIFO and owned fast SPI, stops on
 * FIFO error or timeout, then resets and restores the receive configuration. */
#define LR2021_IQ_TX_MAX_SAMPLES 32767u
#define LR2021_IQ_TX_MAX_US 900000u
#define LR2021_IQ_TX_MAX_REPEATS 16u
#define LR2021_IQ_TX_MAX_RADIUS 56
/* LF PA only. The datasheet LF band; 433.92 and 915 MHz are the tested points. */
#define LR2021_IQ_TX_MIN_HZ 150000000u
#define LR2021_IQ_TX_MAX_HZ 960000000u
/* SetTxParams power in 0.5 dB steps. -18 (-9 dBm) is the tested setting. */
#define LR2021_IQ_TX_MIN_POWER (-19)
#define LR2021_IQ_TX_MAX_POWER 20
#define LR2021_IQ_TX_DEFAULT_POWER (-18)
typedef struct {
    const int8_t *samples;  /* interleaved I,Q */
    size_t pairs;           /* per repetition */
    unsigned repeats;       /* 1..16 */
    uint32_t rate_hz, frequency_hz;
    int power_half_db;
    uint64_t start_us;      /* 0 = immediately, else wait for now_us() >= start_us */
} lr2021_iq_tx;
typedef struct {
    int error, stop_error, recovery_error;
    uint8_t tx_flags;       /* sticky FIFO flags; 0x10 overflow, 0x20 underflow */
    unsigned final_level, min_level, refills;
    uint32_t irq;
    size_t queued_bytes;
    uint64_t started_us, elapsed_us, max_service_gap_us;
} lr2021_iq_tx_result;
bool lr2021_iq_tx_rate_ok(uint32_t rate_hz);
/* Checks the contract above without touching the radio. */
int lr2021_iq_tx_check(const lr2021_iq_tx *);
/* board, when set, supplies the RF switch table applied before TX. The HAL must
 * provide fast and write_fifo. Returns the first of error, stop and recovery. */
int lr2021_iq_transmit(const lr2021_iq_hal *, const lr2021_iq_board *,
                      const lr2021_iq_tx *, lr2021_iq_tx_result *);
#ifdef __cplusplus
}
#endif
#endif
