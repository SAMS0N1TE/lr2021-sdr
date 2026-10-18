#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOARD_RFSW_MAX 6
typedef struct {
    uint8_t dio, modes, pull;
} board_rfsw;
typedef struct {
    uint8_t xl9535, reset_high;
    int8_t reset, busy, irq, irq_dio;
    int8_t sclk, mosi, miso, cs, sda, scl;
    uint8_t tcxo, tcxo_voltage, rfsw_count;
    uint32_t tcxo_ticks;
    board_rfsw rfsw[BOARD_RFSW_MAX];
} board_profile;
typedef int (*board_write)(void *, uint16_t, const uint8_t *, size_t);

void board_profile_defaults(board_profile *p);
const char *board_profile_validate(const board_profile *p, int uart_tx, int uart_rx);
const char *board_profile_edit(board_profile *p, int argc, char **argv);
int board_profile_tcxo(const board_profile *p, board_write write, void *ctx);
int board_profile_rfsw(const board_profile *p, board_write write, void *ctx);
