/* SPDX-License-Identifier: GPL-3.0-only */
#include "radio.h"
#include "board.h"
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* The ESP32-P4 side of lr2021_iq_dev: one SPI device, BUSY, and reset by GPIO
 * or the LilyGO XL9535. The device framing, status checks, setup and receive
 * configuration are the library's (src/lr2021_iq_dev.c). */
static spi_device_handle_t spi;
static SemaphoreHandle_t mutex;
static const board_profile *board;
static i2c_master_dev_handle_t xl;
static lr2021_iq_dev dev;
static lr2021_iq_board chip_board;
static bool bus_owned;
static int spi_hz=CONFIG_LRIQ_SPI_HZ;

static int xl_output(unsigned pin, bool high)
{
    uint8_t reg = 2 + pin / 8, value;
    int e = i2c_master_transmit_receive(xl, &reg, 1, &value, 1, 100);
    if(e) return e;
    uint8_t w[2] = {reg, (value & ~(1u << (pin % 8))) | ((unsigned)high << (pin % 8))};
    e = i2c_master_transmit(xl, w, 2, 100);
    reg = 6 + pin / 8;
    if(!e) e = i2c_master_transmit_receive(xl, &reg, 1, &value, 1, 100);
    if(e) return e;
    w[0] = reg; w[1] = value & ~(1u << (pin % 8));
    return i2c_master_transmit(xl, w, 2, 100);
}
static int bus_transfer(void *ctx, uint8_t *b, size_t n)
{
    (void)ctx;
    spi_transaction_t t = {.length=n*8, .tx_buffer=b, .rx_buffer=b};
    return spi_device_polling_transmit(spi, &t);
}
static int bus_busy(void *ctx) { (void)ctx; return gpio_get_level(board->busy); }
static int bus_reset(void *ctx, bool asserted)
{
    (void)ctx;
    bool level = asserted ? board->reset_high : !board->reset_high;
    return board->xl9535 ? xl_output(14, level) : gpio_set_level(board->reset, level);
}
static int bus_fast(void *ctx, bool on)
{
    (void)ctx;
    if(on) {
        if(bus_owned) return LR2021_IQ_STATE;
        int e = spi_device_acquire_bus(spi, portMAX_DELAY);
        if(!e) bus_owned = true;
        return e;
    }
    if(bus_owned) { spi_device_release_bus(spi); bus_owned = false; }
    return 0;
}
static void bus_lock(void *ctx) { (void)ctx; xSemaphoreTake(mutex, portMAX_DELAY); }
static void bus_unlock(void *ctx) { (void)ctx; xSemaphoreGive(mutex); }
static uint64_t bus_now(void *ctx) { (void)ctx; return esp_timer_get_time(); }
static void bus_sleep(void *ctx, unsigned us)
{
    (void)ctx;
    if(us >= 2000) vTaskDelay(pdMS_TO_TICKS(us / 1000));
    else esp_rom_delay_us(us);
}
static int add_device(int hz)
{
    spi_device_interface_config_t sd = {.clock_speed_hz=hz, .mode=0,
        .spics_io_num=board->cs, .queue_size=1};
    return spi_bus_add_device(SPI2_HOST, &sd, &spi);
}

const lr2021_iq_hal *radio_hal(void) { return &dev.hal; }
lr2021_iq_dev *radio_dev(void) { return &dev; }
const lr2021_iq_board *radio_board(void) { return &chip_board; }
int radio_spi_hz(void) { return spi_hz; }
int radio_set_spi_hz(int hz)
{
    if(hz < 1000000 || hz > 26666667) return LR2021_IQ_ARGUMENT;
    bus_lock(NULL);
    int e = bus_owned ? LR2021_IQ_STATE : spi_bus_remove_device(spi);
    if(!e) e = add_device(hz);
    if(!e) spi_hz = hz;
    bus_unlock(NULL);
    return e;
}
int radio_apply(const lr2021_iq_rx *rx, uint16_t *errors)
{
    int e = lr2021_iq_dev_configure(&dev, rx);
    if(errors) *errors = dev.errors;
    return e;
}
int radio_configure(uint32_t hz)
{
    lr2021_iq_rx rx = dev.rx;
    rx.frequency_hz = hz;
    return radio_apply(&rx, NULL);
}
int radio_info(void)
{
    uint8_t v[2], err[2];
    bus_lock(NULL);
    int e = dev.hal.read(&dev, 0x0101, v, 2);
    if(!e) e = dev.hal.read(&dev, 0x0110, err, 2);
    bus_unlock(NULL);
    if(!e) printf("LR2021 ROM %u.%u frequency=%lu bandwidth=%lu gain=%u boost=%u path=%u configured=%d "
        "errors=%02X%02X spi_khz=%d\n", v[0], v[1], (unsigned long)dev.rx.frequency_hz,
        (unsigned long)dev.rx.bandwidth_hz, dev.rx.gain_step, dev.rx.boost, dev.rx.path,
        dev.profile.configured, err[0], err[1], spi_hz / 1000);
    return e;
}
int radio_init(void)
{
    board = board_active();
    mutex = xSemaphoreCreateMutex();
    if(!mutex) return LR2021_IQ_IO;
    int e;
    if(board->xl9535) {
        i2c_master_bus_handle_t bus;
        i2c_master_bus_config_t bc = {.i2c_port=0, .sda_io_num=board->sda,
            .scl_io_num=board->scl, .clk_source=I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt=7, .flags.enable_internal_pullup=true};
        e = i2c_new_master_bus(&bc, &bus);
        if(e) return e;
        i2c_device_config_t dc = {.dev_addr_length=I2C_ADDR_BIT_LEN_7, .device_address=0x20, .scl_speed_hz=400000};
        e = i2c_master_bus_add_device(bus, &dc, &xl);
        if(!e) e = xl_output(0, false); /* 3.3 V rail */
        if(!e) e = xl_output(1, false); /* MMCX1 */
    } else {
        e = gpio_set_direction(board->reset, GPIO_MODE_OUTPUT);
    }
    if(!e) e = gpio_set_direction(board->busy, GPIO_MODE_INPUT);
    if(!e && board->irq >= 0) e = gpio_set_direction(board->irq, GPIO_MODE_INPUT);
    if(e) return e;
    spi_bus_config_t sb = {.mosi_io_num=board->mosi, .miso_io_num=board->miso,
        .sclk_io_num=board->sclk, .quadwp_io_num=-1, .quadhd_io_num=-1, .max_transfer_sz=258};
    e = spi_bus_initialize(SPI2_HOST, &sb, SPI_DMA_CH_AUTO);
    if(!e) e = add_device(spi_hz);
    if(e) return e;
    chip_board = (lr2021_iq_board){.tcxo=board->tcxo, .tcxo_voltage=board->tcxo_voltage,
        .tcxo_ticks=board->tcxo_ticks, .rfsw_count=board->rfsw_count};
    for(unsigned i = 0; i < board->rfsw_count && i < LR2021_IQ_RFSW_MAX; i++)
        chip_board.rfsw[i] = (lr2021_iq_rfsw){board->rfsw[i].dio, board->rfsw[i].modes, board->rfsw[i].pull};
    const lr2021_iq_bus bus = {.transfer=bus_transfer, .busy=bus_busy, .reset=bus_reset,
        .fast=bus_fast, .lock=bus_lock, .unlock=bus_unlock, .now_us=bus_now, .sleep_us=bus_sleep};
    const lr2021_iq_rx rx = {.frequency_hz=154785000, .bandwidth_hz=31250,
        .gain_step=0, .boost=0, .path=LR2021_IQ_PATH_AUTO};
    return lr2021_iq_dev_open(&dev, &bus, &chip_board, &rx);
}
