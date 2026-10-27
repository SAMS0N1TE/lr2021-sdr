/* SPDX-License-Identifier: GPL-3.0-only */
/* Allocation, accessors and layout checks for the Rust bindings. Large or
 * caller-allocated C structs stay opaque on the Rust side. */
#include "lr2021_bridge.h"
#include "lr2021_iq_tx.h"
#include "lr2021_sim.h"
#include <stdlib.h>
#include <string.h>

typedef struct { lr2021_sim sim; lr2021_iq_dev dev; } lr2021_rs_simdev;
lr2021_rs_simdev *lr2021_rs_simdev_new(int pattern, uint32_t tone_hz)
{
    lr2021_rs_simdev *s=calloc(1,sizeof(*s));
    if(!s)return NULL;
    lr2021_sim_init(&s->sim);
    s->sim.pattern=pattern; if(tone_hz)s->sim.tone_hz=tone_hz;
    return s;
}
void lr2021_rs_simdev_free(lr2021_rs_simdev *s) { if(s){ free(s->sim.tx_capture); free(s); } }
int lr2021_rs_simdev_open(lr2021_rs_simdev *s, const lr2021_iq_board *board, const lr2021_iq_rx *rx)
{
    lr2021_iq_bus bus=lr2021_sim_bus(&s->sim);
    return lr2021_iq_dev_open(&s->dev,&bus,board,rx);
}
lr2021_iq_dev *lr2021_rs_simdev_dev(lr2021_rs_simdev *s) { return &s->dev; }
const lr2021_iq_hal *lr2021_rs_dev_hal(lr2021_iq_dev *d) { return &d->hal; }
uint16_t lr2021_rs_dev_errors(const lr2021_iq_dev *d) { return d->errors; }
int lr2021_rs_simdev_capture(lr2021_rs_simdev *s, size_t capacity)
{
    free(s->sim.tx_capture);
    s->sim.tx_capture=capacity?malloc(capacity):NULL; s->sim.tx_capacity=capacity; s->sim.tx_captured=0;
    return capacity && !s->sim.tx_capture ? -1 : 0;
}
const uint8_t *lr2021_rs_simdev_captured(const lr2021_rs_simdev *s, size_t *n)
{ *n=s->sim.tx_captured; return s->sim.tx_capture; }
uint64_t lr2021_rs_simdev_time_us(lr2021_rs_simdev *s) { return lr2021_sim_now_us(&s->sim); }
void lr2021_rs_simdev_overflow_after(lr2021_rs_simdev *s, uint64_t pairs)
{ s->sim.overflow_after=pairs?s->sim.rx_pairs+pairs:0; }

lr2021_bridge_parser *lr2021_rs_parser_new(void)
{ lr2021_bridge_parser *p=malloc(sizeof(*p)); if(p)lr2021_bridge_parser_init(p); return p; }
void lr2021_rs_parser_free(lr2021_bridge_parser *p) { free(p); }
uint32_t lr2021_rs_parser_crc_errors(const lr2021_bridge_parser *p) { return p->crc_errors; }

/* Sizes of the structs mirrored in Rust, for a layout test. */
size_t lr2021_rs_sizeof(int which)
{
    switch(which) {
    case 0: return sizeof(lr2021_iq_profile);
    case 1: return sizeof(lr2021_iq_result);
    case 2: return sizeof(lr2021_iq_session);
    case 3: return sizeof(lr2021_iq_rx);
    case 4: return sizeof(lr2021_iq_tx);
    case 5: return sizeof(lr2021_iq_tx_result);
    case 6: return sizeof(lr2021_iq_board);
    case 7: return sizeof(lr2021_bridge_hello);
    case 8: return sizeof(lr2021_bridge_rx);
    case 9: return sizeof(lr2021_bridge_rx_ack);
    case 10: return sizeof(lr2021_bridge_tx_begin);
    case 11: return sizeof(lr2021_bridge_rx_data);
    case 12: return sizeof(lr2021_bridge_rx_end);
    case 13: return sizeof(lr2021_bridge_tx_end);
    case 14: return sizeof(lr2021_bridge_frame);
    case 15: return sizeof(lr2021_iq_hal);
    default: return 0;
    }
}
