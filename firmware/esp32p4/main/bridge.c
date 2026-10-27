/* SPDX-License-Identifier: GPL-3.0-only */
#include "bridge.h"
#include "radio.h"
#include "lr2021_bridge_server.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* Transports for the bridge protocol (docs/bridge-protocol.md): the console
 * UART (`bridge [baud]`) and TCP (wifi.c). One session at a time shares one
 * server, one radio worker and one sender. The radio worker runs on core 1,
 * where nothing else is scheduled; the sender and the readers on core 0.
 * Measured on the LilyGO CH343: 4000000 baud is the highest that links (an
 * integer divider of the 80 MHz UART clock); 5000000 does not. */
#define UART UART_NUM_0
#define RX_BUFFER 8192
#define TX_BUFFER 32768

/* 128 queue slots (CMakeLists sets LR2021_BRIDGE_SLOTS), 0.25 s at 250000 pairs/s,
 * in internal RAM: with the queue in PSRAM the radio worker's copies stretched the
 * worst FIFO service gap from about 400 to 761 us, past the 1 KiB FIFO's ~1 ms. */
static lr2021_bridge_server server;
static SemaphoreHandle_t send_lock, session_lock;
static TaskHandle_t worker_task, sender_task;
static uint8_t *tx_buf;
static size_t tx_capacity;
static char board_id[32];
static const bridge_transport *active;

/* Spool: every outgoing frame goes into a PSRAM byte ring and a drain task
 * writes it to the transport, so a link stall (Wi-Fi pauses of about a second
 * were measured) waits in PSRAM instead of overflowing the 0.25 s slot queue.
 * Only the sender and drain tasks on core 0 touch PSRAM; the radio worker on
 * core 1 keeps to internal RAM. When the spool is full, RX_DATA frames are
 * dropped whole (the host sees the first_pair gap); responses wait for room. */
static uint8_t *spool;
static size_t spool_size;
static atomic_size_t spool_head, spool_tail;   /* monotonic byte counts */
static atomic_bool spool_failed;
static uint32_t spool_drops, spool_peak;
static TaskHandle_t drain_task;

static int spool_put(const uint8_t *p, size_t n)
{
    size_t h = atomic_load(&spool_head);
    for(int i = 0; spool_size - (h - atomic_load(&spool_tail)) < n; i++) {
        if(atomic_load(&spool_failed)) return -1;
        if(n >= 4 && p[3] == LR2021_BRIDGE_RX_DATA) { spool_drops++; return 0; }
        if(i >= 1000) return -1;                   /* a response waited 1 s for room */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    size_t at = h % spool_size, first = spool_size - at < n ? spool_size - at : n;
    memcpy(spool + at, p, first);
    memcpy(spool, p + first, n - first);
    atomic_store(&spool_head, h + n);
    size_t used = h + n - atomic_load(&spool_tail);
    if(used > spool_peak) spool_peak = (uint32_t)used;
    xTaskNotifyGive(drain_task);
    return 0;
}
static void drain(void *arg)
{
    (void)arg;
    for(;;) {
        size_t t = atomic_load(&spool_tail), h = atomic_load(&spool_head);
        const bridge_transport *x = active;
        if(h == t || !x) { ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10)); continue; }
        size_t at = t % spool_size, n = h - t;
        if(n > spool_size - at) n = spool_size - at;
        if(n > 16384) n = 16384;
        if(!atomic_load(&spool_failed) && x->send(x->context, spool + at, n)) atomic_store(&spool_failed, true);
        atomic_store(&spool_tail, t + n);
    }
}
static int send_active(void *ctx, const uint8_t *p, size_t n)
{
    (void)ctx;
    const bridge_transport *t = active;
    if(!t) return -1;
    return spool ? spool_put(p, n) : t->send(t->context, p, n);
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
int bridge_init(void)
{
    if(worker_task) return 0;
    send_lock = xSemaphoreCreateMutex();
    session_lock = xSemaphoreCreateMutex();
    if(!send_lock || !session_lock) return -1;
    /* TX upload buffer: PSRAM when the board has it. */
    tx_capacity = LR2021_BRIDGE_TX_CAPACITY;
    tx_buf = heap_caps_malloc(tx_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!tx_buf) tx_buf = heap_caps_malloc(tx_capacity, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!tx_buf) tx_capacity = 0;
    for(spool_size = (size_t)CONFIG_LRIQ_SPOOL_KB * 1024; spool_size >= 65536 && !spool; spool_size /= 2)
        spool = heap_caps_malloc(spool_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!spool) spool_size = 0;
    if(spool && xTaskCreatePinnedToCore(drain, "lrb_drain", 4096, NULL, 9, &drain_task, 0) != pdPASS) {
        heap_caps_free(spool); spool = NULL; spool_size = 0;
    }
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

int bridge_session(const bridge_transport *t)
{
    if(!radio_dev()->profile.lr2021 || bridge_init()) return -1;
    if(xSemaphoreTake(session_lock, 0) != pdTRUE) return -2;
    const lr2021_bridge_io io = {.send=send_active, .lock=send_take, .unlock=send_give, .wake=wake};
    const lr2021_bridge_radio radio = {.hal=radio_hal(), .board=radio_board(), .configure=configure,
        .rx=radio_dev()->rx, .rom_major=(uint8_t)radio_dev()->profile.fw_major,
        .rom_minor=(uint8_t)radio_dev()->profile.fw_minor,
        .caps=LR2021_BRIDGE_CAP_RX|LR2021_BRIDGE_CAP_TX|LR2021_BRIDGE_CAP_TIMED_TX|
              LR2021_BRIDGE_CAP_LIVE_GAIN|LR2021_BRIDGE_CAP_RATE_250K|LR2021_BRIDGE_CAP_CS12|
              LR2021_BRIDGE_CAP_TX_CHECKED|LR2021_BRIDGE_CAP_RAW_ROUTES,
        .name="lr2021-sdr esp32p4 " CONFIG_LRIQ_FW_VERSION, .id=board_id};
    int e = lr2021_bridge_server_init(&server, &io, &radio, tx_buf, tx_capacity);
    /* 1020-byte FIFO reads and one flag check per 4 KiB: the register-path bus
     * pays about 1 us per 64 bytes, so fewer, larger frames carry 500000 pairs/s. */
    if(!e) e = lr2021_bridge_server_tune(&server, 1020, 4096);
    if(!e) {
        atomic_store(&spool_head, 0); atomic_store(&spool_tail, 0); atomic_store(&spool_failed, false);
        spool_drops = spool_peak = 0;
        active = t;
        static uint8_t in[2048];
        while(!lr2021_bridge_server_exit_requested(&server)) {
            int n = t->recv(t->context, in, sizeof(in));
            if(n < 0) break;                          /* transport closed */
            if(n > 0) lr2021_bridge_server_input(&server, in, (size_t)n);
        }
        /* BYE or disconnect: end the stream, let the last frames go out. */
        lr2021_bridge_server_stop(&server);
        for(int i = 0; i < 300 && atomic_load(&server.busy); i++) vTaskDelay(pdMS_TO_TICKS(10));
        for(int i = 0; i < 100 && atomic_load(&server.head) != atomic_load(&server.tail); i++)
            vTaskDelay(pdMS_TO_TICKS(10));
        for(int i = 0; i < 300 && spool && !atomic_load(&spool_failed) &&
            atomic_load(&spool_head) != atomic_load(&spool_tail); i++) vTaskDelay(pdMS_TO_TICKS(10));
        send_take(NULL); active = NULL; send_give(NULL);
        ESP_LOGI("lrbridge", "session end: spool %u KiB, peak %lu B, dropped frames %lu",
                 (unsigned)(spool_size / 1024), (unsigned long)spool_peak, (unsigned long)spool_drops);
    }
    xSemaphoreGive(session_lock);
    return e;
}
bool bridge_busy(void)
{
    if(!session_lock || xSemaphoreTake(session_lock, 0) != pdTRUE) return session_lock != NULL;
    xSemaphoreGive(session_lock);
    return false;
}

static int uart_send(void *ctx, const uint8_t *p, size_t n)
{
    (void)ctx;
    return uart_write_bytes(UART, p, n) == (int)n ? 0 : -1;
}
static int uart_recv(void *ctx, uint8_t *p, size_t n)
{
    (void)ctx;
    int r = uart_read_bytes(UART, p, n, pdMS_TO_TICKS(10));
    return r < 0 ? 0 : r;
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
    if(bridge_init()) { puts("lrbridge: no memory for the bridge"); return 1; }
    if(bridge_busy()) { puts("lrbridge: a TCP session holds the radio"); return 1; }
    printf("lrbridge: baud=%lu\n", baud);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_log_level_set("*", ESP_LOG_NONE); /* nothing but frames on the line */
    int e = uart_mode((int)baud, RX_BUFFER, TX_BUFFER);
    if(!e) {
        const bridge_transport t = {.send=uart_send, .recv=uart_recv};
        e = bridge_session(&t);
    }
    uart_mode(115200, 256, 0);
    esp_log_level_set("*", ESP_LOG_INFO);
    printf("lrbridge: console; result=%d send_errors=%lu rx_crc_errors=%lu spool_peak=%lu spool_drops=%lu\n", e,
           (unsigned long)server.send_errors, (unsigned long)server.parser.crc_errors,
           (unsigned long)spool_peak, (unsigned long)spool_drops);
    return e ? 1 : 0;
}
