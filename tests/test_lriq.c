#include "lriq.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
static const char *summary = "lrcap: source=2 hz=910525000 status=0x030023FC bytes=1024 words=256 phase=readback result=ESP_OK restore=ESP_OK recovery=ESP_OK";
static void rows(lriq_parser *p)
{
    char line[80];
    for (unsigned i = 0; i < 256; i++) {
        snprintf(line, sizeof(line), "lrcap: %03u 0x8000FFFF", i);
        CHECK(lriq_parser_line(p, line) == 0);
    }
}
int main(void)
{
    lriq_parser p;
    lriq_parser_init(&p);
    CHECK(lriq_parser_line(&p, "unrelated console output") == 0);
    CHECK(lriq_parser_line(&p, summary) == -1); /* truncation */
    lriq_parser_init(&p); rows(&p);
    CHECK(lriq_parser_line(&p, summary) == 1);
    int16_t a, b; lriq_components(&p.capture, 0, &a, &b);
    CHECK(a == -1 && b == -32768 && p.capture.frequency_hz == 910525000);
    CHECK(lriq_parser_line(&p, summary) == -1);
    lriq_parser_init(&p);
    CHECK(lriq_parser_line(&p, "lrcap-meta: bw=62500 sample_rate=250000 iq=1") == 0);
    rows(&p); CHECK(lriq_parser_line(&p, summary) == 1);
    CHECK(p.capture.sample_rate_hz == 250000 && p.capture.iq_validated);
    lriq_parser_init(&p);
    CHECK(lriq_parser_line(&p, "lrcap-meta: bw=62500 sample_rate=0 iq=1") == -1);
    lriq_parser_init(&p);
    CHECK(lriq_parser_line(&p, "lrcap-meta: bw=62500 sample_rate=250000 iq=1") == 0);
    CHECK(lriq_parser_line(&p, "lrcap-meta: bw=62500 sample_rate=250000 iq=1") == -1);
    lriq_parser_init(&p);
    CHECK(lriq_parser_line(&p, "lrcap: 000 0x00000000") == 0);
    CHECK(lriq_parser_line(&p, "lrcap: 000 0x00000000") == -1);
    lriq_parser_init(&p); rows(&p);
    CHECK(lriq_parser_line(&p, "lrcap: source=2 hz=910525000 status=0x030023FC bytes=1024 words=256 phase=readback result=ESP_OK restore=ESP_FAIL recovery=ESP_OK") == -1);
    lriq_parser_init(&p); rows(&p);
    CHECK(lriq_parser_line(&p, "lrcap: source=2 hz=910525000 status=0x020023FC bytes=1024 words=256 phase=readback result=ESP_OK restore=ESP_OK recovery=ESP_OK") == -1);
    for (int sign = -1; sign <= 1; sign += 2) {
        lriq_capture c = {0}; c.pairs = LRIQ_PAIRS;
        double f = sign * 0.1137;
        for (unsigned i = 0; i < 256; i++) {
            uint16_t x = (uint16_t)(int16_t)(10000*cos(6.283185307179586*f*i)+300);
            uint16_t y = (uint16_t)(int16_t)(10000*sin(6.283185307179586*f*i)-700);
            c.data[i*4] = (uint8_t)x; c.data[i*4+1] = (uint8_t)(x >> 8);
            c.data[i*4+2] = (uint8_t)y; c.data[i*4+3] = (uint8_t)(y >> 8);
        }
        lriq_tone t; CHECK(lriq_measure_tone(&c, &t) == 0);
        CHECK(fabs(t.cycles_per_pair-f) < 0.0001);
        CHECK(t.peak_to_mean_db > 20);
    }
    lriq_capture zero = {0}; zero.pairs = 256; lriq_tone t;
    CHECK(lriq_measure_tone(&zero, &t) == -1);
    double rate;
    CHECK(lriq_estimate_rate(10000, -0.04, &rate) == 0 && fabs(rate+250000) < 1e-6);
    CHECK(lriq_estimate_rate(10000, 0, &rate) == -1);
    uint8_t fifo_data[8]; lriq_fifo_parser fp;
    const char *fifo_row="lrfifo-hex: 000000 01020304";
    const char *fifo_crc="lrfifo-crc32: B63CFBCD bytes=4 encoding=iq16le";
    const char *fifo_end="lrfifo: bytes=4 max_level=4 polls=1 elapsed_us=10 flags=0x00 result=ESP_OK recovery=ESP_OK";
    lriq_fifo_init(&fp,fifo_data,sizeof(fifo_data));
    CHECK(lriq_fifo_line(&fp,fifo_row)==0);
    CHECK(lriq_fifo_line(&fp,fifo_crc)==0);
    CHECK(lriq_fifo_line(&fp,fifo_end)==1 && fp.bytes==4 && fifo_data[3]==4);
    CHECK(lriq_fifo_line(&fp,fifo_end)==-1);
    lriq_fifo_init(&fp,fifo_data,sizeof(fifo_data));
    CHECK(lriq_fifo_line(&fp,"lrfifo-hex: 000004 01020304")==-1);
    lriq_fifo_init(&fp,fifo_data,sizeof(fifo_data));
    CHECK(lriq_fifo_line(&fp,fifo_row)==0);
    CHECK(lriq_fifo_line(&fp,fifo_end)==-1); /* missing checksum */
    lriq_fifo_init(&fp,fifo_data,sizeof(fifo_data));
    CHECK(lriq_fifo_line(&fp,fifo_row)==0);
    CHECK(lriq_fifo_line(&fp,"lrfifo-crc32: B63CFBCC bytes=4 encoding=iq16le")==-1);
    lriq_fifo_init(&fp,fifo_data,sizeof(fifo_data));
    CHECK(lriq_fifo_line(&fp,fifo_row)==0);
    CHECK(lriq_fifo_line(&fp,fifo_crc)==0);
    CHECK(lriq_fifo_line(&fp,"lrfifo: bytes=4 max_level=4 polls=1 elapsed_us=10 flags=0x10 result=ESP_OK recovery=ESP_OK")==-1);
    lriq_fifo_init(&fp,fifo_data,3);
    CHECK(lriq_fifo_line(&fp,fifo_row)==-1);
    lriq_fifo_init(&fp,fifo_data,sizeof(fifo_data));
    CHECK(lriq_fifo_line(&fp,"lrfifo-hex: 000000 0102030Z")==-1);
    lriq_fifo_init(&fp,fifo_data,sizeof(fifo_data));
    CHECK(lriq_fifo_line(&fp,fifo_row)==0);
    CHECK(lriq_fifo_line(&fp,fifo_crc)==0);
    CHECK(lriq_fifo_line(&fp,"lrfifo: bytes=4 max_level=4 polls=1 elapsed_us=10 flags=0x00 result=ESP_OK recovery=ESP_FAIL")==-1);
    puts("protocol failure cases, FIFO CRC/continuity, endian/sign decoding, tone direction and rate estimation passed");
    return 0;
}
