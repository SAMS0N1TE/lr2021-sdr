#include "radio.h"
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

static spi_device_handle_t spi;
static SemaphoreHandle_t mutex;
static lr2021_iq_profile profile;
static bool bus_owned;
#if CONFIG_LRIQ_LILYGO
static i2c_master_dev_handle_t xl;
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
#endif
static int ready(void)
{
    int64_t end = esp_timer_get_time() + 200000;
    while(gpio_get_level(CONFIG_LRIQ_BUSY)) {
        if(esp_timer_get_time() >= end) return LR2021_IQ_TIMEOUT;
        esp_rom_delay_us(1);
    }
    return 0;
}
static int frame(uint8_t *b, size_t n)
{
    int e = ready();
    if(e) return e;
    spi_transaction_t t = {.length=n*8, .tx_buffer=b, .rx_buffer=b};
    return spi_device_polling_transmit(spi, &t);
}
static int status_ok(const uint8_t *b)
{
    unsigned s = ((unsigned)b[0] << 8) | b[1];
    return s && !(s & 0xF008) && ((s >> 9) & 7) >= 2 &&
        ((s >> 9) & 7) <= 3 && ((s >> 4) & 7) <= 2 && (s & 7) <= 6
        ? 0 : LR2021_IQ_DATA;
}
static int check(void)
{
    uint8_t b[6] = {1, 0};
    int e = frame(b, sizeof(b));
    return e ? e : status_ok(b);
}
static int write_cmd(void *ctx, uint16_t op, const uint8_t *a, size_t n)
{
    (void)ctx;
    if(n > 256) return LR2021_IQ_ARGUMENT;
    uint8_t b[258] = {(uint8_t)(op >> 8), (uint8_t)op};
    if(n) memcpy(b+2, a, n);
    int e = frame(b, n+2);
    return e ? e : check();
}
static int read_args(uint16_t op, const uint8_t *a, size_t n, uint8_t *out, size_t m)
{
    if(n > 256 || m > 256) return LR2021_IQ_ARGUMENT;
    uint8_t b[258] = {(uint8_t)(op >> 8), (uint8_t)op};
    if(n) memcpy(b+2, a, n);
    int e = frame(b, n+2);
    if(e) return e;
    memset(b, 0, m+2);
    e = frame(b, m+2);
    if(!e) e = status_ok(b);
    if(!e) memcpy(out, b+2, m);
    return e;
}
static int read_cmd(void *ctx, uint16_t op, uint8_t *out, size_t n)
{ (void)ctx; return read_args(op, NULL, 0, out, n); }
static int read32(void *ctx, uint32_t addr, uint32_t *value)
{
    (void)ctx;
    uint8_t a[4] = {addr >> 16, addr >> 8, addr, 1}, b[4];
    int e = read_args(0x0106, a, 4, b, 4);
    if(!e) *value = ((uint32_t)b[0]<<24) | ((uint32_t)b[1]<<16) | ((uint32_t)b[2]<<8) | b[3];
    return e;
}
static int write32(void *ctx, uint32_t addr, uint32_t value)
{
    uint8_t a[11] = {addr>>16, addr>>8, addr, 255,255,255,255,
                    value>>24, value>>16, value>>8, value};
    return write_cmd(ctx, 0x0105, a, sizeof(a));
}
static int standby(void *ctx)
{
    uint8_t a = 0;
    int e = write_cmd(ctx, 0x0128, &a, 1);
    if(!e) profile.receiving = false;
    return e;
}
static int receive(void *ctx)
{
    const uint8_t a[3] = {255,255,255};
    int e = write_cmd(ctx, 0x020C, a, 3);
    if(!e) profile.receiving = true;
    return e;
}
static int reset(void *ctx)
{
    profile.configured = false; profile.receiving = false;
#if CONFIG_LRIQ_LILYGO
    int e = xl_output(14, false);
    if(e) return e;
    vTaskDelay(pdMS_TO_TICKS(5));
    e = xl_output(14, true);
#else
    int e = gpio_set_level(CONFIG_LRIQ_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(5));
    if(!e) e = gpio_set_level(CONFIG_LRIQ_RESET, 1);
#endif
    vTaskDelay(pdMS_TO_TICKS(20));
    uint8_t v[2];
    if(!e) e = read_cmd(ctx, 0x0101, v, 2);
    if(e) return e;
    profile.fw_major=v[0]; profile.fw_minor=v[1]; profile.lr2021=v[0]==1;
    if(v[0]!=1 || v[1]!=24) return LR2021_IQ_STATE;
    if(!e) e = standby(ctx);
    if(!e) e = write_cmd(ctx, 0x0111, NULL, 0);
#if CONFIG_LRIQ_TCXO
    const uint8_t tcxo[] = {7,0,0,0x80,0};
    if(!e) e = write_cmd(ctx, 0x0120, tcxo, sizeof(tcxo));
#endif
    uint8_t a=0;
    if(!e) e = write_cmd(ctx, 0x0121, &a, 1); /* LDO */
    a=1;
    if(!e) e = write_cmd(ctx, 0x0206, &a, 1);
    a=0x6F;
    if(!e) e = write_cmd(ctx, 0x0122, &a, 1);
    return e;
}
int radio_configure(uint32_t hz)
{
    if(hz < 150000000 || hz > 960000000) return LR2021_IQ_ARGUMENT;
    int e=standby(NULL);
    profile.configured=false;
#if CONFIG_LRIQ_RFSW
    const uint8_t pins[]={6,7,8,10};
    for(unsigned i=0; !e && i<4; i++) {
        uint8_t a[2]={pins[i],0x23};
        e=write_cmd(NULL,0x0112,a,2);
        a[1]=pins[i]==8?2:0; /* LF RX only */
        if(!e) e=write_cmd(NULL,0x0113,a,2);
    }
#endif
    uint8_t type=0;
    if(!e) e=write_cmd(NULL,0x0207,&type,1);
    unsigned cell=hz/4000000;
    uint8_t cal[6]={cell>>8,cell,0,0,0,0};
    if(!e) e=write_cmd(NULL,0x0123,cal,6);
    uint8_t freq[4]={hz>>24,hz>>16,hz>>8,hz};
    if(!e) e=write_cmd(NULL,0x0200,freq,4);
    const uint8_t path[2]={0,0}, mod[2]={0x72,0x10}, pkt[4]={0,8,255,2}, sync=0x12;
    if(!e) e=write_cmd(NULL,0x0201,path,2);
    if(!e) e=write_cmd(NULL,0x0220,mod,2);
    if(!e) e=write_cmd(NULL,0x0221,pkt,4);
    if(!e) e=write_cmd(NULL,0x0223,&sync,1);
    uint8_t errors[2];
    if(!e) e=read_cmd(NULL,0x0110,errors,2);
    if(!e && (errors[0] || errors[1])) e=LR2021_IQ_DATA;
    if(!e) {
        profile.frequency_hz=hz; profile.bandwidth_hz=31250;
        profile.spreading_factor=7; profile.exclusive_lora=true; profile.configured=true;
    }
    return e;
}
static int restore(void *ctx, const lr2021_iq_profile *saved)
{
    int e=radio_configure(saved->frequency_hz);
    if(!e && saved->receiving) e=receive(ctx);
    return e;
}
static void lock(void *ctx) { (void)ctx; xSemaphoreTake(mutex,portMAX_DELAY); }
static void unlock(void *ctx) { (void)ctx; xSemaphoreGive(mutex); }
static int snapshot(void *ctx, lr2021_iq_profile *p) { (void)ctx; *p=profile; return 0; }
static uint64_t now(void *ctx) { (void)ctx; return esp_timer_get_time(); }
static void sleep_us(void *ctx, unsigned us) { (void)ctx; esp_rom_delay_us(us); }
static int fast(void *ctx, bool on)
{
    (void)ctx;
    if(on) {
        if(bus_owned) return LR2021_IQ_STATE;
        int e=spi_device_acquire_bus(spi,portMAX_DELAY);
        if(!e) bus_owned=true;
        return e;
    }
    if(bus_owned) { spi_device_release_bus(spi); bus_owned=false; }
    return 0;
}
static int clear_fifo(void *ctx) { return write_cmd(ctx,0x011E,NULL,0); }
static int read_fifo(void *ctx, uint8_t *out, size_t n)
{
    (void)ctx;
    if(!n || n>256) return LR2021_IQ_ARGUMENT;
    uint8_t b[258]={0,1};
    int e=frame(b,n+2);
    if(!e) e=status_ok(b);
    if(!e) memcpy(out,b+2,n);
    return e;
}
const lr2021_iq_hal *radio_hal(void)
{
    static const lr2021_iq_hal h={.lock=lock,.unlock=unlock,.snapshot=snapshot,
        .read32=read32,.write32=write32,.write=write_cmd,.read=read_cmd,.standby=standby,
        .receive=receive,.reset=reset,.restore=restore,.fast=fast,.clear_fifo=clear_fifo,
        .read_fifo=read_fifo,.now_us=now,.sleep_us=sleep_us};
    return &h;
}
int radio_info(void)
{
    uint8_t v[2],err[2];
    int e=read_cmd(NULL,0x0101,v,2);
    if(!e) e=read_cmd(NULL,0x0110,err,2);
    if(!e) printf("LR2021 ROM %u.%u frequency=%lu bandwidth=31250 configured=%d errors=%02X%02X\n",
        v[0],v[1],(unsigned long)profile.frequency_hz,profile.configured,err[0],err[1]);
    return e;
}
int radio_init(void)
{
    mutex=xSemaphoreCreateMutex();
    if(!mutex) return LR2021_IQ_IO;
#if CONFIG_LRIQ_LILYGO
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bc={.i2c_port=0,.sda_io_num=CONFIG_LRIQ_SDA,
        .scl_io_num=CONFIG_LRIQ_SCL,.clk_source=I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt=7,.flags.enable_internal_pullup=true};
    int e=i2c_new_master_bus(&bc,&bus);
    if(e) return e;
    i2c_device_config_t dc={.dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=0x20,.scl_speed_hz=400000};
    e=i2c_master_bus_add_device(bus,&dc,&xl);
    if(!e) e=xl_output(0,false); /* 3.3 V rail */
    if(!e) e=xl_output(1,false); /* MMCX1 */
    if(e) return e;
#else
    int e=gpio_set_direction(CONFIG_LRIQ_RESET,GPIO_MODE_OUTPUT);
    if(e) return e;
#endif
    gpio_set_direction(CONFIG_LRIQ_BUSY,GPIO_MODE_INPUT);
    spi_bus_config_t sb={.mosi_io_num=CONFIG_LRIQ_MOSI,.miso_io_num=CONFIG_LRIQ_MISO,
        .sclk_io_num=CONFIG_LRIQ_SCLK,.quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=258};
    e=spi_bus_initialize(SPI2_HOST,&sb,SPI_DMA_CH_AUTO);
    spi_device_interface_config_t sd={.clock_speed_hz=16000000,.mode=0,
        .spics_io_num=CONFIG_LRIQ_CS,.queue_size=1};
    if(!e) e=spi_bus_add_device(SPI2_HOST,&sd,&spi);
    if(!e) e=reset(NULL);
    if(!e) e=radio_configure(154785000);
    return e;
}
