/* SPDX-License-Identifier: GPL-3.0-only */
/* LR2021 on the host's own SPI (Linux spidev) and GPIO (libgpiod v1 or v2)
 * lines, for example a Raspberry Pi. BUSY is an input, NRESET an output driven
 * low to reset. NSS is the spidev chip select. */
#define _DEFAULT_SOURCE
#include "host_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int lrh_parse_board(const lr2021_host_args *a, lr2021_iq_board *b)
{
    static const char *volts[]={"1.6","1.7","1.8","2.2","2.4","2.7","3.0","3.3"};
    memset(b,0,sizeof(*b));
    b->tcxo_ticks=a->tcxo_ticks;
    if(a->tcxo[0] && strcmp(a->tcxo,"off")) {
        unsigned i=0;
        while(i<8 && strcmp(a->tcxo,volts[i]))i++;
        if(i==8)return -1;
        b->tcxo=1; b->tcxo_voltage=(uint8_t)i;
    }
    /* rfsw=dio:modes[:pull],...  modes bits: standby, rx_lf, tx_lf, rx_hf, tx_hf */
    const char *s=a->rfsw;
    while(*s) {
        unsigned dio,modes,pull=3; int n=0;
        if(b->rfsw_count==LR2021_IQ_RFSW_MAX)return -1;
        if(sscanf(s,"%u:%u%n",&dio,&modes,&n)!=2)return -1;
        s+=n;
        if(*s==':'){ if(sscanf(s,":%u%n",&pull,&n)!=1)return -1; s+=n; }
        if(dio<5 || dio>11 || modes>31 || pull>3)return -1;
        b->rfsw[b->rfsw_count++]=(lr2021_iq_rfsw){(uint8_t)dio,(uint8_t)modes,(uint8_t)pull};
        if(*s==',')s++;
        else if(*s)return -1;
    }
    return 0;
}

#if defined(__linux__) && defined(LR2021_HAVE_GPIOD)
#include <errno.h>
#include <fcntl.h>
#include <gpiod.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct {
    int spi; uint32_t hz;
#ifdef LR2021_GPIOD_V2
    struct gpiod_chip *chip; struct gpiod_line_request *req;
#else
    struct gpiod_chip *chip; struct gpiod_line *reset, *busy;
#endif
    unsigned reset_line, busy_line;
} spidev;

static int transfer(void *c, uint8_t *b, size_t n)
{
    spidev *d=c;
    struct spi_ioc_transfer t;
    memset(&t,0,sizeof(t));
    t.tx_buf=(uintptr_t)b; t.rx_buf=(uintptr_t)b; t.len=(uint32_t)n;
    t.speed_hz=d->hz; t.bits_per_word=8;
    return ioctl(d->spi,SPI_IOC_MESSAGE(1),&t)<0 ? -1 : 0;
}
#ifdef LR2021_GPIOD_V2
static int busy(void *c)
{
    spidev *d=c;
    int v=gpiod_line_request_get_value(d->req,d->busy_line);
    return v<0 ? -1 : v==GPIOD_LINE_VALUE_ACTIVE;
}
static int reset(void *c, bool on)
{
    spidev *d=c;
    return gpiod_line_request_set_value(d->req,d->reset_line,
        on?GPIOD_LINE_VALUE_INACTIVE:GPIOD_LINE_VALUE_ACTIVE);
}
#else
static int busy(void *c) { int v=gpiod_line_get_value(((spidev *)c)->busy); return v<0?-1:v; }
static int reset(void *c, bool on) { return gpiod_line_set_value(((spidev *)c)->reset,on?0:1); }
#endif
static uint64_t now(void *c) { (void)c; return lrh_now_us(); }
static void sleep_us(void *c, unsigned us) { (void)c; lrh_sleep_us(us); }

void lrh_spidev_close(void *c)
{
    spidev *d=c;
    if(!d)return;
#ifdef LR2021_GPIOD_V2
    if(d->req)gpiod_line_request_release(d->req);
#else
    if(d->reset)gpiod_line_release(d->reset);
    if(d->busy)gpiod_line_release(d->busy);
#endif
    if(d->chip)gpiod_chip_close(d->chip);
    if(d->spi>=0)close(d->spi);
    free(d);
}
void *lrh_spidev_open(lr2021_iq_bus *bus, const lr2021_host_args *a, char *err, size_t n)
{
    if(a->reset_line<0 || a->busy_line<0 || !a->gpiochip[0]) {
        snprintf(err,n,"local backend needs gpiochip, reset and busy"); return NULL;
    }
    spidev *d=calloc(1,sizeof(*d));
    if(!d)return NULL;
    d->spi=open(a->spidev,O_RDWR);
    d->hz=a->spi_hz?a->spi_hz:16000000; d->reset_line=(unsigned)a->reset_line; d->busy_line=(unsigned)a->busy_line;
    uint8_t mode=SPI_MODE_0, bits=8;
    if(d->spi<0 || ioctl(d->spi,SPI_IOC_WR_MODE,&mode)<0 || ioctl(d->spi,SPI_IOC_WR_BITS_PER_WORD,&bits)<0 ||
       ioctl(d->spi,SPI_IOC_WR_MAX_SPEED_HZ,&d->hz)<0) {
        snprintf(err,n,"%s: %s",a->spidev,strerror(errno)); lrh_spidev_close(d); return NULL;
    }
    char path[160];
    snprintf(path,sizeof(path),"%s%s",strncmp(a->gpiochip,"/dev/",5)?"/dev/":"",a->gpiochip);
#ifdef LR2021_GPIOD_V2
    struct gpiod_line_settings *out=gpiod_line_settings_new(), *in=gpiod_line_settings_new();
    struct gpiod_line_config *lc=gpiod_line_config_new();
    struct gpiod_request_config *rc=gpiod_request_config_new();
    d->chip=gpiod_chip_open(path);
    if(d->chip && out && in && lc && rc) {
        unsigned r=d->reset_line, b=d->busy_line;
        gpiod_line_settings_set_direction(out,GPIOD_LINE_DIRECTION_OUTPUT);
        gpiod_line_settings_set_output_value(out,GPIOD_LINE_VALUE_ACTIVE);
        gpiod_line_settings_set_direction(in,GPIOD_LINE_DIRECTION_INPUT);
        gpiod_request_config_set_consumer(rc,"lr2021-sdr");
        if(!gpiod_line_config_add_line_settings(lc,&r,1,out) && !gpiod_line_config_add_line_settings(lc,&b,1,in))
            d->req=gpiod_chip_request_lines(d->chip,rc,lc);
    }
    if(out)gpiod_line_settings_free(out);
    if(in)gpiod_line_settings_free(in);
    if(lc)gpiod_line_config_free(lc);
    if(rc)gpiod_request_config_free(rc);
    if(!d->req) {
#else
    d->chip=gpiod_chip_open(path);
    if(d->chip) {
        d->reset=gpiod_chip_get_line(d->chip,d->reset_line);
        d->busy=gpiod_chip_get_line(d->chip,d->busy_line);
    }
    if(!d->reset || !d->busy || gpiod_line_request_output(d->reset,"lr2021-sdr",1) ||
       gpiod_line_request_input(d->busy,"lr2021-sdr")) {
        if(d->reset && !gpiod_line_is_requested(d->reset))d->reset=NULL;
        if(d->busy && !gpiod_line_is_requested(d->busy))d->busy=NULL;
#endif
        snprintf(err,n,"%s lines %d/%d: %s",path,a->reset_line,a->busy_line,strerror(errno));
        lrh_spidev_close(d); return NULL;
    }
    *bus=(lr2021_iq_bus){.context=d,.transfer=transfer,.busy=busy,.reset=reset,.now_us=now,.sleep_us=sleep_us};
    return d;
}
#else
void *lrh_spidev_open(lr2021_iq_bus *bus, const lr2021_host_args *a, char *err, size_t n)
{
    (void)bus; (void)a;
    snprintf(err,n,"the local spidev backend needs Linux and libgpiod at build time");
    return NULL;
}
void lrh_spidev_close(void *c) { (void)c; }
#endif
