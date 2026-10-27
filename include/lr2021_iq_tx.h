/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef LR2021_IQ_TX_H
#define LR2021_IQ_TX_H
#include "lr2021_iq_dev.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Finite I/Q burst through ROM 1.24 command 0x0293, selector 0, after OOK
 * setup (docs/tx-discovery.md, docs/tx-playback.md). Signed interleaved I8,Q8.
 * By default the burst must stay inside this repository's tested contract:
 *   rates 4800, 9600, 19200, 38400, 76800, 125000 complex samples/s;
 *   at most 900 ms of samples; 1..16 repetitions; every sample
 *   I*I+Q*Q <= 56*56 (saturation starts near radius 64); LF PA, 150..960 MHz
 *   (433.92 and 915 MHz are the measured points); power -9.5..+10 dBm.
 * LR2021_IQ_TX_UNCHECKED keeps only hardware limits: at most 32767 samples per
 * run including repetitions (the ROM stores a 16-bit byte count for 2 x N),
 * 1..255 repetitions, 1..1000000 samples/s, 100..2740 MHz (the receive
 * synthesizer range), the datasheet SetTxParams range of the selected PA and a
 * SetTx timeout below 512 s. Anything outside the tested contract is then
 * reported in result.untested instead of refused.
 * The burst holds the radio, uses the 1 KiB TX FIFO and owned fast SPI, stops on
 * FIFO error or timeout, then resets and restores the receive configuration.
 * The service loop busy-polls; on an RTOS run it where no idle-task watchdog
 * is watching. */
#define LR2021_IQ_TX_MAX_SAMPLES 32767u
#define LR2021_IQ_TX_MAX_US 900000u
#define LR2021_IQ_TX_MAX_REPEATS 16u
#define LR2021_IQ_TX_MAX_RADIUS 56
#define LR2021_IQ_TX_MIN_HZ 150000000u
#define LR2021_IQ_TX_MAX_HZ 960000000u
/* SetTxParams power in 0.5 dB steps. -18 (-9 dBm) is the tested setting. */
#define LR2021_IQ_TX_MIN_POWER (-19)
#define LR2021_IQ_TX_MAX_POWER 20
#define LR2021_IQ_TX_DEFAULT_POWER (-18)
enum { LR2021_IQ_TX_UNCHECKED=1u };
enum { LR2021_IQ_PA_LF=0, LR2021_IQ_PA_HF=1, LR2021_IQ_PA_AUTO=2 };
/* result.untested bits */
enum {
    LR2021_IQ_TX_UNTESTED_RATE=1u, LR2021_IQ_TX_UNTESTED_RADIUS=2u,
    LR2021_IQ_TX_UNTESTED_DURATION=4u, LR2021_IQ_TX_UNTESTED_FREQUENCY=8u,
    LR2021_IQ_TX_UNTESTED_POWER=16u, LR2021_IQ_TX_UNTESTED_HF_PA=32u,
    LR2021_IQ_TX_UNTESTED_REPEATS=64u
};
typedef struct {
    const int8_t *samples;  /* interleaved I,Q */
    size_t pairs;           /* per repetition */
    unsigned repeats;
    uint32_t rate_hz, frequency_hz;
    int power_half_db;      /* SetTxParams units: LF -19..44, HF -39..24 */
    uint64_t start_us;      /* 0 = immediately, else wait for now_us() >= start_us */
    uint8_t pa, flags;      /* LR2021_IQ_PA_*, LR2021_IQ_TX_UNCHECKED */
} lr2021_iq_tx;
typedef struct {
    int error, stop_error, recovery_error;
    uint8_t tx_flags;       /* sticky FIFO flags; 0x10 overflow, 0x20 underflow */
    unsigned final_level, min_level, refills;
    uint32_t irq, untested;
    size_t queued_bytes;
    uint64_t started_us, elapsed_us, max_service_gap_us;
} lr2021_iq_tx_result;
bool lr2021_iq_tx_rate_ok(uint32_t rate_hz);
unsigned lr2021_iq_tx_pa(const lr2021_iq_tx *);
/* Bits of the tested contract a burst leaves. */
uint32_t lr2021_iq_tx_untested(const lr2021_iq_tx *);
/* Checks the burst without touching the radio. */
int lr2021_iq_tx_check(const lr2021_iq_tx *);
/* board, when set, supplies the RF switch table applied before TX. The HAL must
 * provide fast and write_fifo. Returns the first of error, stop and recovery. */
int lr2021_iq_transmit(const lr2021_iq_hal *, const lr2021_iq_board *,
                      const lr2021_iq_tx *, lr2021_iq_tx_result *);
#ifdef __cplusplus
}
#endif
#endif
