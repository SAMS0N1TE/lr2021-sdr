/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef LR2021_SIM_H
#define LR2021_SIM_H
#include "lr2021_iq_dev.h"
#ifdef __cplusplus
extern "C" {
#endif
/* LR2021 ROM 1.24 model at the SPI-frame level, for host tests without
 * hardware. It implements only the commands this repository uses: status words
 * (previous command status, CMD_DAT on read responses), version, errors, IRQs,
 * FIFO levels/flags, register access for the FIFO mapping, LoRa RX with the
 * source-2 FIFO route at 4 x bandwidth, and the 0x0293 selector-0 TX path that
 * drains the TX FIFO at the configured rate. It does not model RF, filters,
 * calibration quality or the SRAM capture route. Not thread-safe except
 * lr2021_sim_now_us in real-time mode. */
enum { LR2021_SIM_COUNTER=0, LR2021_SIM_TONE=1 };
typedef struct {
    /* clock == NULL selects virtual time: SPI bytes and sleeps advance it. */
    uint64_t (*clock)(void *);
    void (*sleep)(void *, unsigned);
    void *clock_context;
    uint64_t vtime;
    int pattern;            /* COUNTER: I = pair index, Q = index >> 16 */
    uint32_t tone_hz;       /* RF tone for the TONE pattern */
    int16_t tone_amplitude; /* at gain step 13 */
    /* Fault injection: overflow the RX FIFO after this many produced pairs. */
    uint64_t overflow_after;
    /* Real-time hosts can stall the service thread for milliseconds. When set,
     * a stall pauses the simulated radio instead of overflowing its FIFO, so
     * protocol tests do not depend on scheduler luck. Off by default. */
    bool stall_tolerant;
    /* chip */
    bool in_reset, reset_seen;
    uint8_t mode, cmd_status, packet_type, path, boost, step, lora_bw, route;
    uint16_t errors;
    uint32_t irq, freq, rx_base, rx_size, tx_base, tx_size;
    uint8_t resp[256]; size_t resp_len; bool have_resp;
    uint8_t rx_fifo[1100]; unsigned rx_level; uint8_t rx_flags;
    uint64_t rx_t0, rx_pairs, rx_origin;
    double phase; uint32_t noise;
    uint8_t tx_fifo[1100]; unsigned tx_level, tx_head; uint8_t tx_flags;
    uint32_t tx_n, tx_rate; uint8_t tx_selector;
    uint64_t tx_t0, tx_consumed;
    uint8_t *tx_capture; size_t tx_capacity, tx_captured;
    /* statistics */
    unsigned resets, rx_starts, tx_starts, gain_writes, spi_frames;
} lr2021_sim;
void lr2021_sim_init(lr2021_sim *);
lr2021_iq_bus lr2021_sim_bus(lr2021_sim *);
uint64_t lr2021_sim_now_us(lr2021_sim *);
#ifdef __cplusplus
}
#endif
#endif
