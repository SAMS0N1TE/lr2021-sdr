/* SPDX-License-Identifier: GPL-3.0-only */
#include "bridge.h"
#include "radio.h"
#include "lr2021_bridge_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* Serial transport for the bridge protocol (docs/bridge-protocol.md) on the
 * console UART. The console task becomes the reader for the session; the radio
 * worker runs on core 1, where nothing else is scheduled, and the sender on
 * core 0. Measured on the LilyGO CH343: 4000000 baud is the highest that links
 * (an integer divider of the 80 MHz UART clock); 5000000 does not link. */
#define UART UART_NUM_0
#define RX_BUFFER 8192
#define TX_BUFFER 32768

static lr2021_bridge_server server;
static SemaphoreHandle_t send_lock;
static TaskHandle_t worker_task, sender_task;
static uint8_t *tx_buf;
static size_t tx_capacity;
static char board_id[32];

static int uart_send(void *ctx, const uint8_t *p, size_t n)
{
    (void)ctx;
    return uart_write_bytes(UART, p, n) == (int)n ? 0 : -1;
}
static void send_take(void *ctx) { (void)ctx; xSemaphoreTake(send_lock, portMAX_DELAY); }
static void send_give(void *ctx) { (void)ctx; xSemaphoreGive(send_lock); }
static void wake(void *ctx) { (void)ctx; xTaskNotifyGive(worker_task); }
static int configure(void *ctx, const lr2021_iq_rx *rx, uint16_t *errors)
{
    (void)ctx;
    int e = radio_apply(rx, errors);
    if(!e) server.radio.rx = *rx;
    return e;
}
static void worker(void *arg)
{
    (void)arg;
    for(;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        lr2021_bridge_server_work(&server);
    }
}
static void sender(void *arg)
{
    (void)arg;
    for(;;) {
        if(!lr2021_bridge_server_pump(&server)) vTaskDelay(1);
    }
}
static int start_tasks(void)
{
    if(worker_task) return 0;
    send_lock = xSemaphoreCreateMutex();
    if(!send_lock) return -1;
    /* TX upload buffer: PSRAM when the board has it. */
    tx_capacity = LR2021_BRIDGE_TX_CAPACITY;
    tx_buf = heap_caps_malloc(tx_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!tx_buf) tx_buf = heap_caps_malloc(tx_capacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!tx_buf) tx_capacity = 0;
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    snprintf(board_id, sizeof(board_id), "esp32p4-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if(xTaskCreatePinnedToCore(worker, "lrb_radio", 8192, NULL, configMAX_PRIORITIES - 2, &worker_task, 1) != pdPASS)
        return -1;
    if(xTaskCreatePinnedToCore(sender, "lrb_send", 6144, NULL, 10, &sender_task, 0) != pdPASS)
        return -1;
    return 0;
}
static int uart_mode(int baud, size_t rx, size_t tx)
{
    uart_wait_tx_done(UART, pdMS_TO_TICKS(500));
    uart_vfs_dev_use_nonblocking(UART);
    uart_driver_delete(UART);
    int e = uart_driver_install(UART, rx, tx, 0, NULL, 0);
    if(!e) e = uart_set_baudrate(UART, baud);
    if(!e) uart_vfs_dev_use_driver(UART);
    return e;
}

int bridge_command(int argc, char **argv)
{
    unsigned long baud = argc > 1 ? strtoul(argv[1], NULL, 10) : CONFIG_LRIQ_BRIDGE_BAUD;
    if(argc > 2 || baud < 115200 || baud > 5000000) {
        puts("usage: bridge [115200..5000000 baud]"); return 1;
    }
    if(!radio_dev()->profile.lr2021) { puts("lrbridge: radio not initialized"); return 1; }
    if(start_tasks()) { puts("lrbridge: no memory for the bridge"); return 1; }
    const lr2021_bridge_io io = {.send=uart_send, .lock=send_take, .unlock=send_give, .wake=wake};
    const lr2021_bridge_radio radio = {.hal=radio_hal(), .board=radio_board(), .configure=configure,
        .rx=radio_dev()->rx, .rom_major=(uint8_t)radio_dev()->profile.fw_major,
        .rom_minor=(uint8_t)radio_dev()->profile.fw_minor,
        .caps=LR2021_BRIDGE_CAP_RX|LR2021_BRIDGE_CAP_TX|LR2021_BRIDGE_CAP_TIMED_TX|
              LR2021_BRIDGE_CAP_LIVE_GAIN|LR2021_BRIDGE_CAP_RATE_250K,
        .name="lr2021-sdr esp32p4 " CONFIG_LRIQ_FW_VERSION, .id=board_id};
    if(lr2021_bridge_server_init(&server, &io, &radio, tx_buf, tx_capacity)) {
        puts("lrbridge: server init failed"); return 1;
    }
    printf("lrbridge: baud=%lu\n", baud);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_log_level_set("*", ESP_LOG_NONE); /* nothing but frames on the line */
    if(uart_mode((int)baud, RX_BUFFER, TX_BUFFER)) {
        uart_mode(115200, 256, 0);
        puts("lrbridge: UART setup failed"); return 1;
    }
    static uint8_t in[1024];
    while(!lr2021_bridge_server_exit_requested(&server)) {
        int n = uart_read_bytes(UART, in, sizeof(in), pdMS_TO_TICKS(10));
        if(n > 0) lr2021_bridge_server_input(&server, in, (size_t)n);
    }
    /* BYE: let the stream end and the last frames go out, then back to the console. */
    lr2021_bridge_server_stop(&server);
    for(int i = 0; i < 300 && atomic_load(&server.busy); i++) vTaskDelay(pdMS_TO_TICKS(10));
    for(int i = 0; i < 100 && atomic_load(&server.head) != atomic_load(&server.tail); i++) vTaskDelay(pdMS_TO_TICKS(10));
    uart_mode(115200, 256, 0);
    esp_log_level_set("*", ESP_LOG_INFO);
    printf("lrbridge: console; send_errors=%lu rx_crc_errors=%lu\n",
           (unsigned long)server.send_errors, (unsigned long)server.parser.crc_errors);
    return 0;
}
