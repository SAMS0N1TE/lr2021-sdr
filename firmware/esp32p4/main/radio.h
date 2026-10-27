#pragma once
#include "lr2021_iq.h"
#include "lr2021_iq_dev.h"
int radio_init(void);
int radio_configure(uint32_t hz);
/* Applies a full receive configuration (frequency, LoRa BW 31250/62500, gain,
 * boost, path); errors gets GetErrors (IMG_CAL is reported, not refused). */
int radio_apply(const lr2021_iq_rx *rx, uint16_t *errors);
int radio_info(void);
const lr2021_iq_hal *radio_hal(void);
lr2021_iq_dev *radio_dev(void);
const lr2021_iq_board *radio_board(void);
/* Radio SPI clock. 26.67 MHz is the measured ceiling on the LilyGO board
 * (40 MHz fails every transfer) and keeps 250000 pairs/s at a 26% FIFO peak. */
int radio_spi_hz(void);
int radio_set_spi_hz(int hz);
/* `antenna [internal|external]`: the LilyGO SKY13453 route, saved in NVS. */
int radio_antenna_command(int argc, char **argv);
int radio_spibench_command(int argc, char **argv);
int radio_spiclk_command(int argc, char **argv);
