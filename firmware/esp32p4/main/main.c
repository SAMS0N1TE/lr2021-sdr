#include "radio.h"
#include "board.h"
#include "bridge.h"
#include "wifi.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static uint8_t *recording;
static size_t stored, accepted;
static bool save, valid, initialized;
static lr2021_iq_result result;
static int capture_result;
#define RECORDING_CAPACITY 52000u

static bool sink(void *ctx, const uint8_t *data, size_t n, uint64_t first)
{
    (void)ctx;
    if(first != accepted/4 || (n & 3)) return false;
    if(save) {
        if(n > RECORDING_CAPACITY-stored) return false;
        memcpy(recording+stored,data,n); stored+=n;
    }
    accepted+=n;
    return true;
}
static bool number(const char *s, unsigned long low, unsigned long high, unsigned long *out)
{
    char *end;
    unsigned long v=strtoul(s,&end,10);
    if(!*s || *end || v<low || v>high) return false;
    *out=v; return true;
}
static void summary(void)
{
    printf("lrfifo: bytes=%u max_level=%lu polls=%lu elapsed_us=%" PRIu64
        " flags=0x%02X result=%s recovery=%s\n", (unsigned)stored,
        (unsigned long)result.max_fifo_level,(unsigned long)result.polls,result.elapsed_us,
        result.fifo_flags,valid?"ESP_OK":"ESP_FAIL",result.recovery_error?"ESP_FAIL":"ESP_OK");
}
static int capture_cmd(int argc, char **argv)
{
    save=!strcmp(argv[0],"capture");
    unsigned long ms=save?100:3000;
    if(argc>2 || (argc==2 && !number(argv[1],1,save?100:3000,&ms))) {
        printf("usage: %s [1..%u milliseconds]\n",argv[0],save?100:3000); return 1;
    }
    valid=false; stored=accepted=0;
    if(!initialized) { puts("radio initialization failed; reset after checking wiring"); return 1; }
    if(save && !recording) {
        recording=heap_caps_malloc(RECORDING_CAPACITY,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
        if(!recording) { puts("internal RAM allocation failed"); return 1; }
    }
    capture_result=lr2021_iq_stream_checked_fifo(radio_hal(),ms,1024,sink,NULL,&result);
    valid=!capture_result && !result.stopped_by_sink && result.validated_bytes==accepted;
    printf("check: accepted=%u elapsed_us=%" PRIu64 " peak=%lu service_gap_us=%" PRIu64
        " flags=%02X capture=%d recovery=%d rejected=%d result=%s\n",(unsigned)accepted,
        result.elapsed_us,(unsigned long)result.max_fifo_level,result.max_service_gap_us,
        result.fifo_flags,capture_result,result.recovery_error,result.stopped_by_sink,
        valid?"PASS":"FAIL");
    if(!save) valid=false; /* No recording was retained. */
    return capture_result || result.stopped_by_sink;
}
static uint32_t crc32(const uint8_t *p,size_t n)
{
    uint32_t c=0xFFFFFFFFu;
    while(n--) {
        c^=*p++;
        for(unsigned k=0;k<8;k++) c=(c>>1)^((0u-(c&1u))&0xEDB88320u);
    }
    return ~c;
}
static int dump_cmd(int argc,char **argv)
{
    (void)argv;
    if(argc!=1 || !valid || !stored) { puts("run a successful capture first"); return 1; }
    for(size_t off=0;off<stored;off+=64) {
        size_t n=stored-off; if(n>64)n=64;
        printf("lrfifo-hex: %u ",(unsigned)off);
        for(size_t i=0;i<n;i++) printf("%02X",recording[off+i]);
        putchar('\n');
        vTaskDelay(1);
    }
    printf("lrfifo-crc32: %08" PRIX32 " bytes=%u encoding=iq16le\n",crc32(recording,stored),(unsigned)stored);
    summary();
    return 0;
}
static int tune_cmd(int argc,char **argv)
{
    unsigned long hz;
    if(argc!=2 || !number(argv[1],150000000,960000000,&hz)) {
        puts("usage: tune <150000000..960000000 Hz>"); return 1;
    }
    valid=false;
    if(!initialized) return 1;
    int e=radio_configure(hz);
    printf("tune: %lu Hz result=%d\n",hz,e);
    return e;
}
static int info_cmd(int argc,char **argv)
{ (void)argc; (void)argv; return initialized?radio_info():1; }
void app_main(void)
{
    int e=board_load();
    if(!e) e=radio_init();
    initialized=!e;
    printf("LR2021-SDR capture and bridge firmware %s; radio_init=%d\n",CONFIG_LRIQ_FW_VERSION,e);
    if(!e) radio_info();
    esp_console_repl_config_t rc=ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt="lriq> "; rc.task_stack_size=12288; rc.task_priority=configMAX_PRIORITIES-2;
    esp_console_repl_t *repl;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_console_dev_usb_serial_jtag_config_t dc=ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&dc,&rc,&repl));
#else
    esp_console_dev_uart_config_t dc=ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&dc,&rc,&repl));
#endif
    const esp_console_cmd_t commands[]={
        {.command="board",.help="Show or stage board profile; save then reboot to apply",.func=board_command},
        {.command="info",.help="Radio version and errors",.func=info_cmd},
        {.command="tune",.help="Set receive frequency in Hz",.func=tune_cmd},
        {.command="capture",.help="Record 1..100 ms of checked IQ16 in internal RAM",.func=capture_cmd},
        {.command="check",.help="Check FIFO capture for 1..3000 ms; discard samples",.func=capture_cmd},
        {.command="dump",.help="Export the last valid capture as CRC-protected hex",.func=dump_cmd},
        {.command="antenna",.help="Antenna route on LilyGO boards: antenna [internal|external]",.func=radio_antenna_command},
        {.command="wifi",.help="Wi-Fi station and TCP bridge: wifi | wifi set <ssid> <password> | wifi clear",.func=wifi_command},
        {.command="bridge",.help="Serve the binary bridge protocol on this port until BYE",.func=bridge_command}
    };
    for(unsigned i=0;i<sizeof(commands)/sizeof(commands[0]);i++)
        ESP_ERROR_CHECK(esp_console_cmd_register(&commands[i]));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    if(initialized) wifi_start();
}
