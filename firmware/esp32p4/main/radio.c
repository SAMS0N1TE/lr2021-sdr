/* SPDX-License-Identifier: MIT */
#include "radio.h"
#include "board.h"
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "hal/spi_ll.h"
#include "esp_attr.h"
#include "driver/i2c_master.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "nvs.h"
#include <stdlib.h>
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
static bool antenna_internal;

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
/* On the ESP32-P4 a DMA transaction whose buffer or length is not cache-line
 * aligned gets a bounce buffer, a copy and a cache sync each time (about 35-40
 * us), which held FIFO service near 1.3 MB/s. Outside a session the driver sends
 * 64-byte pieces with chip select held. Inside a session, with the bus owned,
 * a long FIFO read sends its cache-line multiple by DMA from aligned buffers and
 * the rest straight through the SPI registers, all under one chip select; other
 * frames use the registers only, one 64-byte piece at a time. */
#define SPI_PIECE 64
#define SPI_DMA_ALIGN 64
#define SPI_DMA_MIN 256
#define SPI_DMA_MAX 1024
static DRAM_ATTR uint32_t piece_buf[SPI_PIECE / 4 + 1];
static uint8_t *dma_tx, *dma_rx;
static void IRAM_ATTR ll_piece(spi_dev_t *hw, uint8_t *b, size_t m, bool keep)
{
    memcpy(piece_buf, b, m);
    spi_ll_dma_rx_enable(hw, false);
    spi_ll_dma_tx_enable(hw, false);
    spi_ll_clear_int_stat(hw);
    spi_ll_enable_mosi(hw, true);
    spi_ll_enable_miso(hw, true);
    spi_ll_set_mosi_bitlen(hw, m * 8);
    spi_ll_set_miso_bitlen(hw, m * 8);
    spi_ll_master_keep_cs(hw, keep);
    spi_ll_write_buffer(hw, (const uint8_t *)piece_buf, m * 8);
    spi_ll_apply_config(hw);
    spi_ll_user_start(hw);
    while(!spi_ll_usr_is_done(hw)) { }
    spi_ll_read_buffer(hw, (uint8_t *)piece_buf, m * 8);
    memcpy(b, piece_buf, m);
}
static int IRAM_ATTR bus_transfer(void *ctx, uint8_t *b, size_t n)
{
    (void)ctx;
    if(bus_owned) {
        spi_dev_t *hw = SPI_LL_GET_HW(SPI2_HOST);
        size_t off = 0;
        /* TX FIFO writes (0x0002) stay on the registers: sent as one continuous
         * DMA burst they lost data and the packet underflowed. */
        if(n >= SPI_DMA_MIN && n <= SPI_DMA_MAX && dma_tx && !(b[0] == 0 && b[1] == 2)) {
            off = n & ~(size_t)(SPI_DMA_ALIGN - 1);
            memcpy(dma_tx, b, off);
            spi_transaction_t t = {.flags=SPI_TRANS_DMA_BUFFER_ALIGN_MANUAL |
                (off < n ? SPI_TRANS_CS_KEEP_ACTIVE : 0),
                .length=off*8, .tx_buffer=dma_tx, .rx_buffer=dma_rx};
            if(spi_device_polling_transmit(spi, &t)) return LR2021_IQ_IO;
            memcpy(b, dma_rx, off);
        }
        for(; off < n; off += SPI_PIECE) {
            const size_t m = n - off > SPI_PIECE ? SPI_PIECE : n - off;
            ll_piece(hw, b + off, m, off + m < n);
        }
        return 0;
    }
    if(spi_device_acquire_bus(spi, portMAX_DELAY)) return LR2021_IQ_IO;
    int e = 0;
    for(size_t off = 0; !e && off < n; off += SPI_PIECE) {
        const size_t m = n - off > SPI_PIECE ? SPI_PIECE : n - off;
        spi_transaction_t t = {.flags=off + m < n ? SPI_TRANS_CS_KEEP_ACTIVE : 0,
            .length=m*8, .tx_buffer=b+off, .rx_buffer=b+off};
        e = spi_device_polling_transmit(spi, &t);
    }
    spi_device_release_bus(spi);
    return e;
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
        /* One driver transaction loads this device's clock, mode and chip select
         * into the peripheral before the register path takes over. */
        spi_transaction_t prime = {0};
        if(!e) e = spi_device_polling_transmit(spi, &prime);
        if(!e) bus_owned = true;
        else spi_device_release_bus(spi);
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
static int spi_delay_ns;
static int add_device(int hz)
{
    spi_device_interface_config_t sd = {.clock_speed_hz=hz, .mode=0,
        .spics_io_num=board->cs, .queue_size=1, .input_delay_ns=spi_delay_ns};
    return spi_bus_add_device(SPI2_HOST, &sd, &spi);
}

const lr2021_iq_hal *radio_hal(void) { return &dev.hal; }
lr2021_iq_dev *radio_dev(void) { return &dev; }
const lr2021_iq_board *radio_board(void) { return &chip_board; }
int radio_spi_hz(void) { return spi_hz; }
int radio_set_spi_hz(int hz)
{
    if(hz < 1000000 || hz > 30000000) return LR2021_IQ_ARGUMENT;
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
        /* SKY13453 VCTL on XL9535 IO1: high is the internal antenna (RF1), low
         * the MMCX1 jack. Saved with `antenna`; MMCX1 unless set otherwise. */
        nvs_handle_t h; uint8_t internal = 0;
        if(!nvs_open("radio", NVS_READONLY, &h)) { nvs_get_u8(h, "ant_int", &internal); nvs_close(h); }
        antenna_internal = internal;
        if(!e) e = xl_output(1, antenna_internal);
    } else {
        e = gpio_set_direction(board->reset, GPIO_MODE_OUTPUT);
    }
    if(!e) e = gpio_set_direction(board->busy, GPIO_MODE_INPUT);
    if(!e && board->irq >= 0) e = gpio_set_direction(board->irq, GPIO_MODE_INPUT);
    if(e) return e;
    spi_bus_config_t sb = {.mosi_io_num=board->mosi, .miso_io_num=board->miso,
        .sclk_io_num=board->sclk, .quadwp_io_num=-1, .quadhd_io_num=-1, .max_transfer_sz=SPI_DMA_MAX};
    e = spi_bus_initialize(SPI2_HOST, &sb, SPI_DMA_CH_AUTO);
    if(!e) {
        dma_tx = spi_bus_dma_memory_alloc(SPI2_HOST, SPI_DMA_MAX, 0);
        dma_rx = spi_bus_dma_memory_alloc(SPI2_HOST, SPI_DMA_MAX, 0);
        if(!dma_tx || !dma_rx) { free(dma_tx); free(dma_rx); dma_tx = dma_rx = NULL; }
    }
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

int radio_antenna_command(int argc, char **argv)
{
    if(argc == 2 && (!strcmp(argv[1], "internal") || !strcmp(argv[1], "external"))) {
        if(!board->xl9535) { puts("antenna: this board has no LilyGO antenna switch"); return 1; }
        bool internal = !strcmp(argv[1], "internal");
        bus_lock(NULL);
        int e = xl_output(1, internal);
        bus_unlock(NULL);
        nvs_handle_t h;
        if(!e && !nvs_open("radio", NVS_READWRITE, &h)) {
            nvs_set_u8(h, "ant_int", internal); nvs_commit(h); nvs_close(h);
        }
        if(!e) antenna_internal = internal;
        printf("antenna: %s result=%d\n", antenna_internal ? "internal (RF1)" : "external (MMCX1)", e);
        return e;
    }
    if(argc == 1) {
        printf("antenna: %s; receive and transmit both use it. Do not transmit on MMCX1 with nothing fitted.\n",
               board->xl9535 ? (antenna_internal ? "internal (RF1)" : "external (MMCX1)") : "fixed");
        return 0;
    }
    puts("usage: antenna [internal|external]");
    return 1;
}

/* `spibench [count]`: time the radio bus as a FIFO session uses it (level read,
 * 256-byte FIFO read, 2-byte flag read), with the bus owned. For tuning only. */
int radio_spibench_command(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 200;
    if(n < 1 || n > 10000) { puts("usage: spibench [1..10000]"); return 1; }
    static uint8_t data[256];
    uint8_t two[2];
    bus_lock(NULL);
    int e = dev.hal.fast(&dev, true);
    int64_t t0 = esp_timer_get_time();
    for(int i = 0; !e && i < n; i++) e = dev.hal.read(&dev, 0x011C, two, 2);
    int64_t t1 = esp_timer_get_time();
    for(int i = 0; !e && i < n; i++) e = dev.hal.read_fifo(&dev, data, 256);
    int64_t t2 = esp_timer_get_time();
    for(int i = 0; !e && i < n; i++) e = dev.hal.read_fifo(&dev, data, 64);
    int64_t t3 = esp_timer_get_time();
    dev.hal.fast(&dev, false);
    bus_unlock(NULL);
    double lv = (double)(t1 - t0) / n, f256 = (double)(t2 - t1) / n, f64 = (double)(t3 - t2) / n;
    printf("spibench: n=%d level_read=%.1f us fifo_read256=%.1f us (%.2f MB/s) fifo_read64=%.1f us (%.2f MB/s) "
           "per 1 KiB with a level read per 256 B: %.0f us (%.2f MB/s) spi_khz=%d result=%d\n",
           n, lv, f256, 256 / f256, f64, 64 / f64, 4 * (lv + f256), 1024 / (4 * (lv + f256)), spi_hz / 1000, e);
    return e;
}

/* `spiclk [hz [input_delay_ns]]`: radio SPI clock and MISO input delay, then a
 * GetVersion read as a check. For tuning only; not saved. */
int radio_spiclk_command(int argc, char **argv)
{
    if(argc > 1) {
        int hz = atoi(argv[1]);
        if(argc > 2) spi_delay_ns = atoi(argv[2]);
        int e = radio_set_spi_hz(hz);
        if(e) { printf("spiclk: %d Hz refused (%d)\n", hz, e); return 1; }
    }
    int actual = 0; spi_device_get_actual_freq(spi, &actual);
    uint8_t v[2] = {0};
    bus_lock(NULL);
    int e = dev.hal.read(&dev, 0x0101, v, 2);
    bus_unlock(NULL);
    printf("spiclk: actual_khz=%d input_delay_ns=%d version=%u.%u result=%d\n", actual, spi_delay_ns, v[0], v[1], e);
    return e;
}
