/* SPDX-License-Identifier: GPL-3.0-only */
#include "wifi.h"
#include "bridge.h"
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Wi-Fi station through the board's ESP32-C6 (esp_hosted over SDIO) and the
 * bridge protocol on TCP port 51021, one connection at a time. Credentials
 * are kept in NVS ("wifi" namespace) and set from the console. */
#define PORT 51021

static esp_netif_t *sta;
static bool started, connected;
static char ip[16];
static TaskHandle_t server_task;

static bool load(wifi_config_t *c)
{
    nvs_handle_t h;
    if(nvs_open("wifi", NVS_READONLY, &h)) return false;
    size_t n = sizeof(c->sta.ssid), m = sizeof(c->sta.password);
    bool ok = !nvs_get_str(h, "ssid", (char *)c->sta.ssid, &n) &&
              !nvs_get_str(h, "pass", (char *)c->sta.password, &m);
    nvs_close(h);
    return ok && c->sta.ssid[0];
}
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if(base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if(base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        connected = false; ip[0] = 0;
        esp_wifi_connect();
    } else if(base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&e->ip_info.ip));
        connected = true;
        printf("wifi: connected ip=%s bridge=tcp:%d\n", ip, PORT);
    }
}

typedef struct { int fd; } tcp_ctx;
static int tcp_send(void *ctx, const uint8_t *p, size_t n)
{
    tcp_ctx *c = ctx;
    while(n) {
        int r = send(c->fd, p, n, 0);
        if(r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}
static int tcp_recv(void *ctx, uint8_t *p, size_t n)
{
    tcp_ctx *c = ctx;
    int r = recv(c->fd, p, n, 0);
    if(r > 0) return r;
    if(r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    return -1;                                       /* closed or failed */
}
static void tcp_server(void *arg)
{
    (void)arg;
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family=AF_INET, .sin_port=htons(PORT), .sin_addr.s_addr=htonl(INADDR_ANY)};
    if(ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof(a)) || listen(ls, 1)) {
        printf("wifi: TCP listen on %d failed\n", PORT); vTaskDelete(NULL);
    }
    for(;;) {
        struct sockaddr_in peer; socklen_t len = sizeof(peer);
        int fd = accept(ls, (struct sockaddr *)&peer, &len);
        if(fd < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        if(bridge_busy()) { close(fd); continue; }   /* the serial bridge holds the radio */
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        struct timeval tv = {.tv_sec=0, .tv_usec=10000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int buf = 65535;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
        tcp_ctx c = {fd};
        const bridge_transport t = {.context=&c, .send=tcp_send, .recv=tcp_recv};
        bridge_session(&t);
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}

int wifi_start(void)
{
    wifi_config_t c = {0};
    if(started || !load(&c)) return started ? 0 : 1;
    int e = esp_netif_init();
    if(!e || e == ESP_ERR_INVALID_STATE) e = esp_event_loop_create_default();
    if(e == ESP_ERR_INVALID_STATE) e = 0;
    if(!e) { sta = esp_netif_create_default_wifi_sta(); e = sta ? 0 : -1; }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if(!e) e = esp_wifi_init(&init);
    if(!e) e = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    if(!e) e = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    if(!e) e = esp_wifi_set_mode(WIFI_MODE_STA);
    if(!e) e = esp_wifi_set_config(WIFI_IF_STA, &c);
    if(!e) e = esp_wifi_set_ps(WIFI_PS_NONE);     /* throughput over power */
    if(!e) e = esp_wifi_start();
    if(!e && !server_task && bridge_init() == 0)
        xTaskCreatePinnedToCore(tcp_server, "lrb_tcp", 6144, NULL, 9, &server_task, 0);
    started = !e;
    printf("wifi: start ssid=\"%s\" result=%d\n", (const char *)c.sta.ssid, e);
    return e;
}

int wifi_command(int argc, char **argv)
{
    if(argc == 4 && !strcmp(argv[1], "set")) {
        if(strlen(argv[2]) > 31 || strlen(argv[3]) > 63) { puts("wifi: ssid <= 31, password <= 63"); return 1; }
        nvs_handle_t h;
        int e = nvs_open("wifi", NVS_READWRITE, &h);
        if(!e) e = nvs_set_str(h, "ssid", argv[2]);
        if(!e) e = nvs_set_str(h, "pass", argv[3]);
        if(!e) e = nvs_commit(h);
        if(!e) nvs_close(h);
        printf("wifi: saved \"%s\" result=%d%s\n", argv[2], e, started ? "; reboot to use it" : "");
        return e || started ? e : wifi_start();
    }
    if(argc == 2 && !strcmp(argv[1], "clear")) {
        nvs_handle_t h;
        int e = nvs_open("wifi", NVS_READWRITE, &h);
        if(!e) { nvs_erase_all(h); nvs_commit(h); nvs_close(h); }
        printf("wifi: cleared result=%d\n", e);
        return e;
    }
    if(argc == 1) {
        printf("wifi: started=%d connected=%d ip=%s bridge=tcp:%d session=%d\n",
               started, connected, ip[0] ? ip : "-", PORT, bridge_busy());
        return 0;
    }
    puts("usage: wifi | wifi set <ssid> <password> | wifi clear");
    return 1;
}
