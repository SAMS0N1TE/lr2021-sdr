#include "debug.h"
#include "radio.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define RING (128u * 1024u)
#define BLOCK 4096u
typedef struct { uint32_t power, gain_reg, clipped; } block_stat;
static uint8_t *ring;
static uint32_t ring_len, ring_start;
static char summary[320];
static block_stat stats[512];
static uint32_t stat_count;
static uint32_t busy_limit_us = 200000; /* longer while a PRAM sweep runs on the chip */

static int frame(uint8_t *b, size_t n)
{
    lr2021_iq_dev *d = radio_dev();
    uint64_t end = d->bus.now_us(d->bus.context) + busy_limit_us;
    for(;;) {
        int busy = d->bus.busy(d->bus.context);
        if(busy < 0) return LR2021_IQ_IO;
        if(!busy) break;
        if(d->bus.now_us(d->bus.context) >= end) return LR2021_IQ_TIMEOUT;
    }
    return d->bus.transfer(d->bus.context, b, n) ? LR2021_IQ_IO : 0;
}
static int raw_cmd(uint16_t op, const uint8_t *in, size_t n, uint8_t *out, size_t m, unsigned *stat)
{
    uint8_t b[300] = {(uint8_t)(op >> 8), (uint8_t)op};
    if(n > sizeof(b) - 2 || m > sizeof(b) - 2) return LR2021_IQ_ARGUMENT;
    if(n) memcpy(b + 2, in, n);
    int e = frame(b, n + 2);
    if(e) return e;
    size_t k = m ? m + 2 : 6;
    memset(b, 0, k);
    if(!m) b[0] = 0x01;
    e = frame(b, k);
    if(e) return e;
    unsigned s = ((unsigned)b[0] << 8) | b[1];
    if(stat) *stat = s;
    if(m && out) memcpy(out, b + 2, m);
    return ((s >> 9) & 7) >= 2 ? 0 : LR2021_IQ_DATA;
}
static int put(uint16_t op, const uint8_t *in, size_t n) { return raw_cmd(op, in, n, NULL, 0, NULL); }
static int mem_read(uint32_t addr, uint32_t *w, unsigned count)
{
    uint8_t a[4] = {(uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)count}, r[128];
    int e = raw_cmd(0x0106, a, 4, r, 4u * count, NULL);
    for(unsigned i = 0; i < count; i++)
        w[i] = ((uint32_t)r[4*i] << 24) | ((uint32_t)r[4*i+1] << 16) | ((uint32_t)r[4*i+2] << 8) | r[4*i+3];
    return e;
}
static int mem_write(uint32_t addr, uint32_t v)
{
    const uint8_t a[11] = {(uint8_t)(addr >> 16), (uint8_t)(addr >> 8), (uint8_t)addr, 255, 255, 255, 255,
        (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    return put(0x0105, a, sizeof(a));
}
static void lock(void) { const lr2021_iq_hal *h = radio_hal(); h->lock(h->context); }
static void unlock(void) { const lr2021_iq_hal *h = radio_hal(); h->unlock(h->context); }
static int recover(void)
{
    const lr2021_iq_hal *h = radio_hal();
    lr2021_iq_profile p = {0};
    int e = h->snapshot(h->context, &p);
    if(!e) e = h->reset(h->context);
    if(!e) e = h->restore(h->context, &p);
    return e;
}
static void dump(uint32_t addr, uint32_t n)
{
    uint32_t w[32];
    while(n) {
        unsigned chunk = n > 32 ? 32 : n;
        int e = mem_read(addr, w, chunk);
        for(unsigned i = 0; i < chunk; i += 8) {
            printf("%06lX:", (unsigned long)(addr + 4 * i));
            for(unsigned j = i; j < chunk && j < i + 8; j++) printf(" %08lX", (unsigned long)w[j]);
            if(e) printf(" !st=%d", e);
            putchar('\n');
        }
        addr += 4 * chunk;
        n -= chunk;
    }
}
static int mr_cmd(int argc, char **argv)
{
    if(argc < 2 || argc > 3) { puts("usage: mr <hex addr> [words]"); return 1; }
    uint32_t addr = strtoul(argv[1], NULL, 16), n = argc > 2 ? strtoul(argv[2], NULL, 0) : 1;
    lock(); dump(addr, n ? n : 1); unlock();
    puts("mr done");
    return 0;
}
static int mw_cmd(int argc, char **argv)
{
    if(argc != 3) { puts("usage: mw <hex addr> <hex value>"); return 1; }
    lock();
    int e = mem_write(strtoul(argv[1], NULL, 16), strtoul(argv[2], NULL, 16));
    unlock();
    printf("mw st=%d\n", e);
    return e != 0;
}
static int rc_cmd(int argc, char **argv)
{
    if(argc < 3) { puts("usage: rc <hex op> <reply bytes> [hex param bytes...]"); return 1; }
    uint8_t in[64], out[128] = {0};
    size_t n = 0, m = strtoul(argv[2], NULL, 0);
    for(int i = 3; i < argc && n < sizeof(in); i++) in[n++] = (uint8_t)strtoul(argv[i], NULL, 16);
    if(m > sizeof(out)) m = sizeof(out);
    unsigned stat = 0;
    lock();
    int e = raw_cmd((uint16_t)strtoul(argv[1], NULL, 16), in, n, out, m, &stat);
    unlock();
    printf("rc st=%d stat=%04X", e, stat);
    for(size_t i = 0; i < m; i++) printf(" %02X", out[i]);
    putchar('\n');
    return 0;
}
static int err_cmd(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint8_t r[2] = {0};
    lock();
    int e = raw_cmd(0x0110, NULL, 0, r, 2, NULL);
    if(!e) e = put(0x0111, NULL, 0);
    unlock();
    printf("err st=%d errors=0x%X\n", e, ((unsigned)r[0] << 8) | r[1]);
    return e != 0;
}
static int romprobe_cmd(int argc, char **argv)
{
    (void)argc; (void)argv;
    static const uint32_t bases[] = {0x000000, 0x100000, 0x200000, 0x400000, 0x600000, 0x700000, 0x800000 - 0x101000 + 0x1000,
        0x900000, 0xA00000, 0xC00000, 0xE00000, 0xF00000, 0xF80000};
    static const uint32_t offs[] = {0x0000, 0x24E0, 0xB3A4};
    lock();
    for(unsigned i = 0; i < sizeof(bases) / sizeof(bases[0]); i++)
        for(unsigned j = 0; j < sizeof(offs) / sizeof(offs[0]); j++)
            if(bases[i] + offs[j] <= 0xFFFFFF) dump(bases[i] + offs[j], 4);
    unlock();
    puts("romprobe done");
    return 0;
}
static int rxrestore_cmd(int argc, char **argv)
{
    (void)argc; (void)argv;
    lock();
    int e = recover();
    unlock();
    printf("rxrestore st=%d\n", e);
    return e != 0;
}
static int gain_cmd(int argc, char **argv)
{
    (void)argc; (void)argv;
    lr2021_iq_gain_state g;
    int e = lr2021_iq_read_gain(radio_hal(), &g);
    printf("gain st=%d step=%u manual=%u value=%u rssi_raw=%u (%.1f dBm)\n", e, g.step, g.manual, g.gain_value,
           g.rssi_raw, -(double)g.rssi_raw / 2);
    return 0;
}
static int cap_cmd(int argc, char **argv)
{
    if(argc < 4 || argc > 5) { puts("usage: cap <hex cfg> <words> <ms> [hex trig]"); return 1; }
    uint8_t cfg = (uint8_t)strtoul(argv[1], NULL, 16), trig = argc > 4 ? (uint8_t)strtoul(argv[4], NULL, 16) : 0xFF;
    uint32_t words = strtoul(argv[2], NULL, 0), ms = strtoul(argv[3], NULL, 0);
    if(words > 2048) words = 2048;
    const lr2021_iq_hal *h = radio_hal();
    const uint8_t stby = 0, rx[3] = {0xFF, 0xFF, 0xFF}, stop[6] = {0xFF, 0, 0, 0, 0, 0};
    const uint8_t c[10] = {trig, 0, 0, 0, 0, cfg, 0, 0, (uint8_t)(words >> 8), (uint8_t)words};
    uint8_t ptr[2];
    lock();
    for(uint32_t i = 0; i < words; i++) mem_write(0x804000 + 4 * i, 0xA5A5A5A5);
    put(0x0128, &stby, 1);
    int s1 = put(0x0290, c, sizeof(c));
    int s2 = put(0x020C, rx, sizeof(rx));
    printf("cap cfg=%02X cfgst=%d rxst=%d ptr:", cfg, s1, s2);
    uint64_t t0 = h->now_us(h->context), tc = 0;
    unsigned last = 0xFFFF;
    while(h->now_us(h->context) - t0 < (uint64_t)ms * 1000) {
        if(raw_cmd(0x0291, NULL, 0, ptr, 2, NULL)) { printf(" ERR"); break; }
        unsigned v = ((unsigned)ptr[0] << 8) | ptr[1];
        uint64_t now = h->now_us(h->context);
        if(v != last) { printf(" %lu:%04X", (unsigned long)((now - t0) / 1000), v); last = v; tc = now; }
        if(tc && now - tc > 200000) break;
    }
    putchar('\n');
    int s3 = put(0x0290, stop, sizeof(stop));
    int s4 = put(0x0128, &stby, 1);
    printf("cap stopst=%d stbyst=%d\n", s3, s4);
    dump(0xF30044, 6);
    uint32_t fresh = 0, w[32];
    for(uint32_t i = 0; i < words; i += 32) {
        unsigned n = words - i > 32 ? 32 : words - i;
        mem_read(0x804000 + 4 * i, w, n);
        for(unsigned j = 0; j < n; j++) fresh += w[j] != 0xA5A5A5A5;
    }
    printf("cap fresh=%lu/%lu\n", (unsigned long)fresh, (unsigned long)words);
    if(fresh) dump(0x804000, words);
    int r = recover();
    unlock();
    printf("cap done restore=%d\n", r);
    return 0;
}
static int capwait_cmd(int argc, char **argv)
{
    if(argc != 4) { puts("usage: capwait <hex cfg> <ratio> <ms>"); return 1; }
    uint8_t cfg = (uint8_t)strtoul(argv[1], NULL, 16);
    uint32_t ratio = strtoul(argv[2], NULL, 0), ms = strtoul(argv[3], NULL, 0);
    enum { WORDS = 2048 };
    static uint32_t buf[WORDS];
    const lr2021_iq_hal *h = radio_hal();
    const uint8_t stby = 0, rx[3] = {0xFF, 0xFF, 0xFF}, stop[6] = {0xFF, 0, 0, 0, 0, 0};
    const uint8_t c[10] = {0xFF, 0, 0, 0, 0, cfg, 0, 0, (uint8_t)(WORDS >> 8), (uint8_t)WORDS};
    uint8_t ptr[2] = {0};
    uint64_t base = 0, t0 = h->now_us(h->context);
    uint32_t n = 0;
    bool hit = false;
    lock();
    while(!hit && h->now_us(h->context) - t0 < (uint64_t)ms * 1000) {
        put(0x0128, &stby, 1);
        put(0x0290, c, sizeof(c));
        put(0x020C, rx, sizeof(rx));
        uint64_t tw = h->now_us(h->context);
        while(h->now_us(h->context) - tw < 40000) {
            if(raw_cmd(0x0291, NULL, 0, ptr, 2, NULL)) break;
            if((((unsigned)ptr[0] << 8) | ptr[1]) >= 0x2000) break;
        }
        put(0x0290, stop, sizeof(stop));
        put(0x0128, &stby, 1);
        for(uint32_t i = 0; i < WORDS; i += 32) mem_read(0x804000 + 4 * i, &buf[i], 32);
        uint64_t pw = 0;
        for(uint32_t i = 0; i < WORDS; i++)
            for(int b = 0; b < 4; b++) { int8_t v = (int8_t)(buf[i] >> (8 * b)); pw += (uint64_t)(v * v); }
        n++;
        unsigned long t = (unsigned long)((h->now_us(h->context) - t0) / 1000);
        printf("capwait n=%lu t=%lu pw=%llu ptr=%02X%02X\n", (unsigned long)n, t, (unsigned long long)pw, ptr[0], ptr[1]);
        if(n == 1) { base = pw ? pw : 1; printf("capwait base=%llu\n", (unsigned long long)base); continue; }
        if(pw > base * ratio) {
            hit = true;
            printf("capwait hit n=%lu t=%lu pw=%llu (%llux)\n", (unsigned long)n, t, (unsigned long long)pw,
                   (unsigned long long)(pw / base));
            for(uint32_t i = 0; i < WORDS; i += 8) {
                printf("%06lX:", (unsigned long)(0x804000 + 4 * i));
                for(int j = 0; j < 8; j++) printf(" %08lX", (unsigned long)buf[i + j]);
                putchar('\n');
            }
        }
    }
    if(!hit) printf("capwait timeout n=%lu\n", (unsigned long)n);
    int r = recover();
    unlock();
    printf("capwait done restore=%d\n", r);
    return 0;
}
static int stream_cmd(int argc, char **argv)
{
    if(argc < 5 || argc > 7) { puts("usage: stream <src> <ratio> <ms> <pre KiB> [unused] [dec 1..8]"); return 1; }
    if(!ring && !(ring = heap_caps_malloc(RING, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT))) { puts("stream: no PSRAM ring"); return 1; }
    uint8_t src = (uint8_t)strtoul(argv[1], NULL, 0);
    uint32_t ratio = strtoul(argv[2], NULL, 0), ms = strtoul(argv[3], NULL, 0), pre = strtoul(argv[4], NULL, 0) * 1024;
    uint32_t dec = argc > 6 ? strtoul(argv[6], NULL, 0) : 1;
    if(dec < 1 || dec > 8) dec = 1;
    if(pre > RING) pre = RING;
    const lr2021_iq_hal *h = radio_hal();
    void *c = h->context;
    const uint8_t stby = 0, raw = 0x0B, rx[3] = {0xFF, 0xFF, 0xFF}, clr[2] = {0x3F, 0x3F};
    const uint8_t route[11] = {0xFF, 0, 0, 0, 0, (uint8_t)(0x08 | (src & 7)), 0, 0, 0, 0, 1};
    lock();
    int fe = h->fast(c, true);
    put(0x0128, &stby, 1);
    put(0x0207, &raw, 1);
    int s1 = put(0x0290, route, sizeof(route));
    mem_write(0xF30028, 0x804400);
    mem_write(0xF30030, 0x3E8);
    put(0x0114, clr, 2);
    int s2 = put(0x020C, rx, sizeof(rx));
    put(0x011E, NULL, 0);
    put(0x0114, clr, 2);
    uint32_t w = 0, total = 0, blkpw = 0, blkn = 0, nblk = 0, maxlvl = 0, polls = 0, clipped = 0;
    int32_t acc_i = 0, acc_q = 0; uint32_t acc_n = 0;
    uint8_t base_gain = 0, trigger_gain = 0, lv[2], fl[2] = {0};
    bool gain_trigger = false, hit = false;
    uint64_t base = 0, t0 = h->now_us(c);
    int64_t stop_at = -1;
    stat_count = 0;
    static uint8_t tmp[1020];
    while(h->now_us(c) - t0 < (uint64_t)ms * 1000) {
        if(raw_cmd(0x011C, NULL, 0, lv, 2, NULL)) break;
        polls++;
        uint32_t level = (((uint32_t)lv[0] << 8) | lv[1]) & ~3u;
        if(level > maxlvl) maxlvl = level;
        while(level) {
            uint32_t n = level > sizeof(tmp) ? sizeof(tmp) : level;
            if(h->read_fifo(c, tmp, n)) { level = 0; break; }
            level -= n;
            uint32_t m = 0;
            for(uint32_t i = 0; i < n; i += 2) {
                int8_t vi = (int8_t)tmp[i], vq = (int8_t)tmp[i + 1];
                clipped += (vi <= -127 || vi >= 126) + (vq <= -127 || vq >= 126);
                acc_i += vi; acc_q += vq;
                if(++acc_n == dec) {
                    tmp[m++] = (uint8_t)(int8_t)(acc_i / (int32_t)dec);
                    tmp[m++] = (uint8_t)(int8_t)(acc_q / (int32_t)dec);
                    acc_i = acc_q = 0; acc_n = 0;
                }
            }
            for(uint32_t i = 0; i < m; i++) {
                int8_t v = (int8_t)tmp[i];
                ring[w] = tmp[i];
                w = (w + 1) % RING;
                blkpw += (uint32_t)(v * v);
                if(++blkn == BLOCK) {
                    nblk++;
                    uint32_t reg = 0;
                    bool valid = !mem_read(0xF30154, &reg, 1);
                    uint8_t g = (reg >> 20) & 15;
                    bool automatic = ((reg >> 16) & 15) == 0;
                    valid = valid && g >= 1 && g <= 13;
                    if(stat_count < 512) stats[stat_count++] = (block_stat){blkpw, reg, clipped};
                    clipped = 0;
                    if(nblk > 4 && nblk <= 36) {
                        base += blkpw;
                        if(valid && automatic && g > base_gain) base_gain = g;
                    } else if(nblk > 36 && ratio && !hit) {
                        if(valid && automatic && g > base_gain) base_gain = g;
                        bool drop = valid && automatic && base_gain && g < base_gain;
                        if(drop || (uint64_t)blkpw * 32 > base * ratio) {
                            hit = true; gain_trigger = drop; trigger_gain = g;
                            stop_at = (int64_t)total + i + 1 + (RING - pre);
                        }
                    }
                    blkpw = 0; blkn = 0;
                }
            }
            total += m;
        }
        if(hit && (int64_t)total >= stop_at) break;
        if((polls & 1023) == 0) vTaskDelay(1);
    }
    uint32_t el = (uint32_t)((h->now_us(c) - t0) / 1000);
    raw_cmd(0x011B, NULL, 0, fl, 2, NULL);
    put(0x0128, &stby, 1);
    if(!fe) h->fast(c, false);
    int r = recover();
    unlock();
    ring_len = total < RING ? total : RING;
    ring_start = total < RING ? 0 : w;
    snprintf(summary, sizeof(summary),
             "stream src=%u dec=%lu routest=%d rxst=%d bytes=%lu ms=%lu Bps=%lu maxlvl=%lu polls=%lu fifoflags=%02X hit=%d base=%lu gainbase=%u trigger=%s gain=%u restore=%d",
             src, (unsigned long)dec, s1, s2, (unsigned long)total, (unsigned long)el,
             (unsigned long)(el ? (uint64_t)total * 1000 / el : 0), (unsigned long)maxlvl, (unsigned long)polls, fl[0], hit,
             (unsigned long)(base / 32), base_gain, hit ? (gain_trigger ? "gain" : "power") : "none", trigger_gain, r);
    puts("stream done");
    return 0;
}
static int streamdump_cmd(int argc, char **argv)
{
    (void)argc; (void)argv;
    puts(summary);
    printf("STREAMBIN %lu\n", (unsigned long)ring_len);
    fflush(stdout);
    for(uint32_t i = 0; ring && i < ring_len;) {
        uint32_t k = (ring_start + i) % RING, n = ring_len - i;
        if(n > 512) n = 512;
        if(n > RING - k) n = RING - k;
        uart_write_bytes(CONFIG_ESP_CONSOLE_UART_NUM, ring + k, n);
        i += n;
    }
    uart_wait_tx_done(CONFIG_ESP_CONSOLE_UART_NUM, pdMS_TO_TICKS(30000));
    putchar('\n');
    puts("stream done");
    return 0;
}
static int streamstats_cmd(int argc, char **argv)
{
    (void)argc; (void)argv;
    puts("block power gain manual clipped");
    for(uint32_t i = 0; i < stat_count; i++)
        printf("%lu %lu %lu %lu %lu\n", (unsigned long)(i + 1), (unsigned long)stats[i].power,
               (unsigned long)((stats[i].gain_reg >> 20) & 15), (unsigned long)((stats[i].gain_reg >> 16) & 15),
               (unsigned long)stats[i].clipped);
    puts("streamstats done");
    return 0;
}
static int agcrssi_cmd(int argc, char **argv)
{
    unsigned count = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 20;
    unsigned interval = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 50;
    if(argc > 3 || !count || count > 10000 || interval > 10000) { puts("usage: agcrssi [count] [interval_ms]"); return 1; }
    const lr2021_iq_hal *h = radio_hal();
    lock();
    int e = h->receive(h->context);
    uint64_t t0 = h->now_us(h->context);
    puts("agcrssi: t_ms,inst_b0,inst_b1,inst_raw,inst_dbm,field_raw,step,manual,gain_value,match");
    for(unsigned i = 0; !e && i < count; i++) {
        uint8_t r[2] = {0};
        uint32_t f = 0, agc = 0, value = 0;
        e = raw_cmd(0x020B, NULL, 0, r, 2, NULL);
        if(!e) e = mem_read(0xF301A8, &f, 1);
        if(!e) e = mem_read(0xF30154, &agc, 1);
        if(!e) e = mem_read(0xF3012C, &value, 1);
        if(e) break;
        unsigned inst = ((unsigned)r[0] << 1) | (r[1] & 1u), field = (f >> 18) & 0x1FF;
        printf("agcrssi: %llu,%u,%u,%u,%.1f,%u,%lu,%lu,%lu,%d\n", (unsigned long long)((h->now_us(h->context) - t0) / 1000),
               r[0], r[1], inst, -(double)inst / 2, field, (unsigned long)((agc >> 20) & 15), (unsigned long)((agc >> 16) & 15),
               (unsigned long)((value >> 18) & 0x1FF), inst == field);
        if(interval) h->sleep_us(h->context, interval * 1000u);
    }
    int r = recover();
    unlock();
    printf("agcrssi: done error=%d restore=%d\n", e, r);
    return e || r;
}
static int bw_code(uint32_t bw) { return bw == 31250 ? 2 : bw == 62500 ? 3 : bw == 125000 ? 4 : bw == 250000 ? 5 : -1; }
static int pktstatus_cmd(int argc, char **argv)
{
    unsigned seconds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 0) : 60;
    uint32_t freq = argc > 2 ? (uint32_t)strtoul(argv[2], NULL, 0) : 910525000u;
    uint32_t bw = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 62500u;
    unsigned sf = argc > 4 ? (unsigned)strtoul(argv[4], NULL, 0) : 7, sync = argc > 5 ? (unsigned)strtoul(argv[5], NULL, 0) : 0x12;
    unsigned gain = argc > 6 ? (unsigned)strtoul(argv[6], NULL, 0) : 0, preamble = argc > 7 ? (unsigned)strtoul(argv[7], NULL, 0) : 16;
    int code = bw_code(bw);
    if(argc > 8 || !seconds || seconds > 3600 || freq < 150000000u || freq > 960000000u || code < 0 || sf < 5 || sf > 12 ||
       sync > 255 || gain > 13 || preamble < 6 || preamble > 65535) {
        puts("usage: pktstatus [seconds] [freq_hz] [bw] [sf] [sync] [gain 0=AGC] [preamble]");
        return 1;
    }
    const lr2021_iq_hal *h = radio_hal();
    void *c = h->context;
    const uint8_t type = 0, step = (uint8_t)gain, sw = (uint8_t)sync, path[2] = {0, 0};
    const uint8_t f[4] = {(uint8_t)(freq >> 24), (uint8_t)(freq >> 16), (uint8_t)(freq >> 8), (uint8_t)freq};
    const uint8_t mod[2] = {(uint8_t)((sf << 4) | (unsigned)code), (uint8_t)(0x10 | (sf >= 11 && bw <= 125000 ? 1 : 0))};
    const uint8_t pkt[4] = {(uint8_t)(preamble >> 8), (uint8_t)preamble, 255, 2};
    const uint8_t all[4] = {0xFF, 0xFF, 0xFF, 0xFF}, forever[3] = {0xFF, 0xFF, 0xFF};
    lock();
    int e = h->standby(c);
    if(!e) e = put(0x0207, &type, 1);
    if(!e) e = put(0x0200, f, 4);
    if(!e) e = put(0x0201, path, 2);
    if(!e) e = put(0x021A, &step, 1);
    if(!e) e = put(0x0220, mod, 2);
    if(!e) e = put(0x0221, pkt, 4);
    if(!e) e = put(0x0223, &sw, 1);
    if(!e) e = put(0x0116, all, 4);
    if(!e) e = put(0x011E, NULL, 0);
    if(!e) e = put(0x020C, forever, 3);
    printf("pktstatus: %lu Hz bw %lu sf %u sync 0x%02X gain %u preamble %u for %u s\n", (unsigned long)freq,
           (unsigned long)bw, sf, sync, gain, preamble, seconds);
    puts("pktstatus: t_ms,irq,len,b0,b1,b2,b3,b4,b5,b6,b7,b8,b9,gain_b9,step_now,rssi_dbm,snr_db,fei_raw");
    uint64_t t0 = h->now_us(c);
    unsigned packets = 0;
    while(!e && h->now_us(c) - t0 < (uint64_t)seconds * 1000000u) {
        uint8_t q[4] = {0};
        e = raw_cmd(0x0117, NULL, 0, q, 4, NULL);
        uint32_t irq = ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) | ((uint32_t)q[2] << 8) | q[3];
        if(e || !(irq & ((1u << 18) | (1u << 22) | (1u << 9)))) { h->sleep_us(c, 2000); continue; }
        uint8_t s[10] = {0}, l[2] = {0}, payload[256];
        uint32_t agc = 0;
        e = raw_cmd(0x022A, NULL, 0, s, 10, NULL);
        if(!e) e = mem_read(0xF30154, &agc, 1);
        if(!e) e = raw_cmd(0x0212, NULL, 0, l, 2, NULL);
        unsigned len = ((unsigned)l[0] << 8) | l[1];
        if(!e && (irq & (1u << 18)) && len && len <= 255) e = h->read_fifo(c, payload, len);
        if(!e) e = put(0x011E, NULL, 0);
        if(e) break;
        packets++;
        const unsigned rssi = ((unsigned)s[3] << 1) | ((s[5] >> 1) & 1u);
        const uint32_t fei = ((uint32_t)s[6] << 16) | ((uint32_t)s[7] << 8) | s[8];
        printf("pktstatus: %llu,0x%08lX,%u,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%02X,%u,%lu,%.1f,%.2f,%lu\n",
               (unsigned long long)((h->now_us(c) - t0) / 1000), (unsigned long)irq, len, s[0], s[1], s[2], s[3], s[4], s[5],
               s[6], s[7], s[8], s[9], s[9], (unsigned long)((agc >> 20) & 15), -(double)rssi / 2, (double)(int8_t)s[2] / 4,
               (unsigned long)fei);
    }
    int r = recover();
    unlock();
    printf("pktstatus: %u packets, error=%d restore=%d\n", packets, e, r);
    return e || r;
}
static void undoc_print(const char *what, int e, unsigned stat, const uint8_t *r, size_t m)
{
    printf("undocprobe: %s error=%d stat=0x%04X cmd=%u data=", what, e, stat, (stat >> 9) & 7);
    for(size_t i = 0; i < m; i++) printf("%02X", r[i]);
    putchar('\n');
}
static int undocprobe_cmd(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "all";
    bool all = !strcmp(what, "all");
    if(argc > 2 || (!all && strcmp(what, "aux") && strcmp(what, "param") && strcmp(what, "getters"))) {
        puts("usage: undocprobe [all|aux|param|getters]");
        return 1;
    }
    const lr2021_iq_hal *h = radio_hal();
    uint8_t r[32]; unsigned st = 0; int e;
    lock();
    if(all || !strcmp(what, "aux")) {
        const uint8_t a[4] = {0, 0, 0x02, 0x90};
        memset(r, 0, sizeof(r)); st = 0;
        e = raw_cmd(0x0103, a, 4, r, 4, &st);
        undoc_print("0x0103 aux 0x290 jli_base", e, st, r, 4);
    }
    if(all || !strcmp(what, "param"))
        for(uint8_t i = 0x0B; i <= 0x15; i++) {
            char label[40];
            memset(r, 0, sizeof(r)); st = 0;
            e = raw_cmd(0x0213, &i, 1, r, 1, &st);
            snprintf(label, sizeof(label), "0x0213 get 0x%02X", i);
            undoc_print(label, e, st, r, 1);
        }
    if(all || !strcmp(what, "getters"))
        for(int pass = 0; pass < 2; pass++) {
            int x = pass ? h->receive(h->context) : h->standby(h->context);
            memset(r, 0, sizeof(r)); st = 0;
            e = x ? x : raw_cmd(0x023E, NULL, 0, r, 6, &st);
            undoc_print(pass ? "0x023E in RX" : "0x023E in standby", e, st, r, 6);
            memset(r, 0, sizeof(r)); st = 0;
            e = x ? x : raw_cmd(0x023F, NULL, 0, r, 18, &st);
            undoc_print(pass ? "0x023F in RX" : "0x023F in standby", e, st, r, 18);
        }
    int rr = recover();
    unlock();
    printf("undocprobe: done restore=%d\n", rr);
    return rr != 0;
}

/* Programs on the radio's own CPU, from lr2021-re (third_party/lr2021-re/firmware/pram). pram.bin
 * goes into the 4 KB Patch RAM and adds ActivatePram (0x012D) sub-commands: 1 = spectrum sweep,
 * 2 = hello, 9 = capture + 256-point FFT + dB on the chip. Nothing here loads Semtech's patch, so
 * all of PRAM is free. A radio reset clears it; the commands below load it again when needed. */
extern const uint8_t pram_bin_start[] asm("_binary_pram_bin_start");
extern const uint8_t pram_bin_end[] asm("_binary_pram_bin_end");
#define PRAM_HOST 0x801000u /* write-only window; reading it stalls the chip */
#define PRAM_CPU 0x101000u
#define PRAM_SWEEP_OUT 0x101C80u
#define PRAM_FFT_DB 0x807B00u
static uint32_t pram_word(unsigned i)
{
    const uint8_t *p = pram_bin_start + 4 * i;
    size_t left = (size_t)(pram_bin_end - p);
    uint32_t w = 0;
    for(unsigned b = 0; b < 4 && b < left; b++) w |= (uint32_t)p[b] << (8 * b);
    return w;
}
static bool pram_hello(void)
{
    const uint8_t p[2] = {2, 0xAB};
    uint8_t r[6] = {0};
    return !raw_cmd(0x012D, p, 2, r, 6, NULL) && !memcmp(r, "LRC1", 4);
}
/* Call with the bus locked. Standby, write the blob, ActivatePram, read it back. */
static int pram_load(bool verbose)
{
    const unsigned words = (unsigned)((pram_bin_end - pram_bin_start + 3) / 4);
    const uint8_t stby = 0, act = 0;
    int e = put(0x0128, &stby, 1);
    for(unsigned i = 0; !e && i < words; i++) e = mem_write(PRAM_HOST + 4 * i, pram_word(i));
    if(e) { if(verbose) printf("pram: write failed st=%d\n", e); return e; }
    /* Activate before reading back: pram_check switches the bank mapping first. */
    e = put(0x012D, &act, 1);
    unsigned bad = 0;
    for(unsigned i = 0; !e && i < words; i += 32) {
        uint32_t w[32];
        unsigned n = words - i > 32 ? 32 : words - i;
        e = mem_read(PRAM_CPU + 4 * i, w, n);
        for(unsigned j = 0; !e && j < n; j++) bad += w[j] != pram_word(i + j);
    }
    if(!e && bad) e = LR2021_IQ_DATA;
    if(!e && !pram_hello()) e = LR2021_IQ_DATA;
    if(verbose) printf("pram: %u words activate+verify st=%d mismatched=%u\n", words, e, bad);
    return e;
}
static int pram_ready(void) { return pram_hello() ? 0 : pram_load(false); }
static int pram_sweep(int argc, char **argv)
{
    if(argc < 5 || argc > 7) { puts("usage: pram sweep <start Hz> <stop Hz> <step Hz> [dwell 1000] [avg 4]"); return 1; }
    uint32_t start = strtoul(argv[2], NULL, 0), stop = strtoul(argv[3], NULL, 0), step = strtoul(argv[4], NULL, 0);
    uint32_t dwell = argc > 5 ? strtoul(argv[5], NULL, 0) : 1000, avg = argc > 6 ? strtoul(argv[6], NULL, 0) : 4;
    if(!step || stop < start || !avg || avg > 64 || dwell > 65535 || (stop - start) / step >= 20000) {
        puts("pram sweep: bad range"); return 1;
    }
    const lr2021_iq_hal *h = radio_hal();
    uint32_t total = (stop - start) / step + 1, done = 0;
    lock();
    int e = pram_ready();
    if(!e) e = h->receive(h->context); /* RSSI follows SetRfFrequency in continuous receive */
    uint64_t t0 = h->now_us(h->context);
    busy_limit_us = 5000000;
    puts("pram sweep: freq_hz,rssi_dbm");
    while(!e && done < total) {
        uint32_t n = total - done > 400 ? 400 : total - done, f = start + done * step;
        const uint8_t p[15] = {1, (uint8_t)(f >> 24), (uint8_t)(f >> 16), (uint8_t)(f >> 8), (uint8_t)f,
            (uint8_t)(step >> 24), (uint8_t)(step >> 16), (uint8_t)(step >> 8), (uint8_t)step,
            (uint8_t)(n >> 8), (uint8_t)n, (uint8_t)(dwell >> 8), (uint8_t)dwell, (uint8_t)(avg >> 8), (uint8_t)avg};
        uint8_t r[2];
        e = raw_cmd(0x012D, p, sizeof(p), r, 2, NULL);
        uint32_t got = ((uint32_t)r[0] << 8) | r[1], w[32];
        if(!e && got != n) e = LR2021_IQ_DATA;
        for(uint32_t i = 0; !e && i < n; i += 64) {
            uint32_t k = n - i > 64 ? 64 : n - i;
            e = mem_read(PRAM_SWEEP_OUT + 2 * i, w, (k + 1) / 2);
            for(uint32_t j = 0; !e && j < k; j++) {
                uint32_t v = (w[j / 2] >> (16 * (j & 1))) & 0xFFFF;
                printf("%lu,%.1f\n", (unsigned long)(f + (i + j) * step), -(double)v / (2.0 * avg));
            }
        }
        done += n;
    }
    busy_limit_us = 200000;
    uint32_t ms = (uint32_t)((h->now_us(h->context) - t0) / 1000);
    int r = recover();
    unlock();
    printf("pram sweep done points=%lu ms=%lu error=%d restore=%d\n", (unsigned long)done, (unsigned long)ms, e, r);
    return e || r;
}
static int pram_cmd(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "";
    if(!strcmp(what, "sweep")) return pram_sweep(argc, argv);
    if(argc != 2 || (strcmp(what, "load") && strcmp(what, "hello"))) {
        puts("usage: pram load | pram hello | pram sweep <start Hz> <stop Hz> <step Hz> [dwell] [avg]");
        return 1;
    }
    lock();
    int e = !strcmp(what, "load") ? pram_load(true) : (pram_hello() ? 0 : LR2021_IQ_DATA);
    unlock();
    printf("pram %s st=%d\n", what, e);
    return e != 0;
}
/* wfrun <nseg 1..10> [frames 0=until x] [overlap 0|1] [baud] [bandwidth Hz]: the chip captures raw
 * I/Q, runs the FFT and returns 256 half-dB bins per frame; this loop only reads them back and
 * streams them. Same frames as lr2021-re's console: "WFBIN <nseg> <rate> <freq>", then per frame
 * A5 5A, seq (u16 LE), t_us (u32 LE), chip_us (u32 LE), 256 x u8 half-dB (bin 0 = DC, 128..255
 * negative); 'x' stops it, then "wfrun done ...". The 115200 console carries ~40 frames/s; with
 * [baud] the UART switches after the WFBIN line and back after the last frame. */
static int wfrun_cmd(int argc, char **argv)
{
    uint32_t nseg = argc > 1 ? strtoul(argv[1], NULL, 0) : 1, frames = argc > 2 ? strtoul(argv[2], NULL, 0) : 0;
    uint32_t ov = argc > 3 ? strtoul(argv[3], NULL, 0) : 0, baud = argc > 4 ? strtoul(argv[4], NULL, 0) : 0;
    uint32_t bw = argc > 5 ? strtoul(argv[5], NULL, 0) : 0;
    if(argc < 2 || argc > 6 || !nseg || nseg > 10 || ov > 1 || (ov && nseg != 1) || (baud && (baud < 115200 || baud > 6000000)) ||
       (bw && bw != 31250 && bw != 62500)) {
        puts("usage: wfrun <nseg 1..10> [frames] [overlap 0|1, nseg 1] [baud] [bandwidth 31250|62500]");
        return 1;
    }
    lr2021_iq_dev *d = radio_dev();
    if(bw && bw != d->rx.bandwidth_hz) {
        lr2021_iq_rx rx = d->rx;
        rx.bandwidth_hz = bw;
        int e = radio_apply(&rx, NULL);
        if(e) { printf("wfrun error bandwidth st=%d\n", e); return 1; }
    }
    const lr2021_iq_hal *h = radio_hal();
    void *c = h->context;
    const uint8_t stby = 0, raw = 0x0B;
    lock();
    int fe = h->fast(c, true);
    int e = pram_ready();
    if(!e) e = put(0x0128, &stby, 1);
    if(!e) e = put(0x0207, &raw, 1);
    if(e) {
        if(!fe) h->fast(c, false);
        int r = recover();
        unlock();
        printf("wfrun error setup st=%d restore=%d\n", e, r);
        return 1;
    }
    printf("WFBIN %lu %lu %lu\n", (unsigned long)nseg, (unsigned long)(4 * d->rx.bandwidth_hz), (unsigned long)d->rx.frequency_hz);
    fflush(stdout);
    uart_wait_tx_done(CONFIG_ESP_CONSOLE_UART_NUM, pdMS_TO_TICKS(1000));
    if(baud) { uart_set_baudrate(CONFIG_ESP_CONSOLE_UART_NUM, baud); vTaskDelay(pdMS_TO_TICKS(100)); }
    static uint8_t out[12 + 256];
    uint32_t n = 0, sum_chip = 0, sum_read = 0, sum_write = 0, w[32];
    uint8_t rsp[4] = {0};
    while(!frames || n < frames) {
        uint8_t ch;
        if(uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &ch, 1, 0) == 1 && ch == 'x') break;
        uint64_t t0 = h->now_us(c);
        const uint8_t p[3] = {9, (uint8_t)nseg, (uint8_t)ov};
        e = raw_cmd(0x012D, p, 3, rsp, 4, NULL);
        uint64_t t1 = h->now_us(c);
        if(e || !rsp[1]) { if(!e) e = LR2021_IQ_DATA; break; }
        for(unsigned half = 0; !e && half < 2; half++) {
            e = mem_read(PRAM_FFT_DB + 128 * half, w, 32);
            for(unsigned i = 0; i < 128; i++) out[12 + 128 * half + i] = (uint8_t)(w[i / 4] >> (8 * (i & 3)));
        }
        if(e) break;
        uint64_t t2 = h->now_us(c);
        uint32_t ts = (uint32_t)t0, chip = (uint32_t)(t1 - t0);
        out[0] = 0xA5; out[1] = 0x5A; out[2] = (uint8_t)n; out[3] = (uint8_t)(n >> 8);
        memcpy(out + 4, &ts, 4);
        memcpy(out + 8, &chip, 4);
        uart_write_bytes(CONFIG_ESP_CONSOLE_UART_NUM, out, sizeof(out));
        sum_chip += chip; sum_read += (uint32_t)(t2 - t1); sum_write += (uint32_t)(h->now_us(c) - t2);
        n++;
        if((n & 255) == 0) vTaskDelay(1);
    }
    if(ov) { const uint8_t s[2] = {9, 0}; raw_cmd(0x012D, s, 2, rsp, 0, NULL); } /* stop the capture left running */
    uart_wait_tx_done(CONFIG_ESP_CONSOLE_UART_NUM, pdMS_TO_TICKS(5000));
    if(baud) { vTaskDelay(pdMS_TO_TICKS(100)); uart_set_baudrate(CONFIG_ESP_CONSOLE_UART_NUM, CONFIG_ESP_CONSOLE_UART_BAUDRATE); }
    if(!fe) h->fast(c, false);
    int r = recover();
    unlock();
    if(baud) vTaskDelay(pdMS_TO_TICKS(600)); /* the host switches back after 0.3 s of quiet */
    if(e) printf("\nwfrun error status=%d seg=%u frames=%lu restore=%d\n", e, rsp[1], (unsigned long)n, r);
    else printf("\nwfrun done %lu chip_us %lu read_us %lu write_us %lu restore=%d\n", (unsigned long)n,
                (unsigned long)(n ? sum_chip / n : 0), (unsigned long)(n ? sum_read / n : 0), (unsigned long)(n ? sum_write / n : 0), r);
    return e || r;
}

void debug_register(void)
{
    const esp_console_cmd_t commands[] = {
        {.command="mr", .help="Debug: ReadRegMem32 words at a hex address. Unmapped addresses and the PRAM window 0x801000 stall the chip", .func=mr_cmd},
        {.command="mw", .help="Debug: WriteRegMem32 one word. Can overwrite PRAM, live RAM or registers; rxrestore afterwards", .func=mw_cmd},
        {.command="rc", .help="Debug: raw command <hex op> <reply bytes> [hex params]. Some opcodes change or hang the chip", .func=rc_cmd},
        {.command="err", .help="Debug: print and clear the chip error register", .func=err_cmd},
        {.command="romprobe", .help="Debug: read a few words at candidate ROM bases", .func=romprobe_cmd},
        {.command="rxrestore", .help="Debug: reset the radio and reapply the saved receive configuration", .func=rxrestore_cmd},
        {.command="gain", .help="Chip gain step, manual setting, gain value and RSSI field", .func=gain_cmd},
        {.command="cap", .help="Debug: one-shot 0x0290 capture into RAM 0x804000: cap <hex cfg> <words> <ms> [hex trig]", .func=cap_cmd},
        {.command="capwait", .help="Debug: repeat one-shot captures until power > ratio x the first: capwait <hex cfg> <ratio> <ms>", .func=capwait_cmd},
        {.command="stream", .help="Debug: packet type 0x0B FIFO stream into a 128 KiB ring, power or AGC trigger: stream <src> <ratio> <ms> <pre KiB> [-] [dec]", .func=stream_cmd},
        {.command="streamdump", .help="Debug: summary, then STREAMBIN <n> and n raw bytes of the last stream", .func=streamdump_cmd},
        {.command="streamstats", .help="Debug: per-4 KiB block power, gain step, manual setting, rail counts of the last stream", .func=streamstats_cmd},
        {.command="agcrssi", .help="GetRssiInst against the 0xF301A8 field and the AGC registers: agcrssi [count] [interval_ms]", .func=agcrssi_cmd},
        {.command="pktstatus", .help="Receive LoRa packets, print all 10 GetLoraPacketStatus bytes: pktstatus [s] [Hz] [bw] [sf] [sync] [gain] [preamble]", .func=pktstatus_cmd},
        {.command="undocprobe", .help="Read-only probes of undocumented opcodes 0x0103, 0x0213, 0x023E, 0x023F", .func=undocprobe_cmd},
        {.command="pram", .help="lr2021-re's on-chip programs in Patch RAM: pram load | hello | sweep <start> <stop> <step Hz> [dwell] [avg]", .func=pram_cmd},
        {.command="wfrun", .help="Stream the spectrum the chip computes itself: wfrun <nseg> [frames] [overlap] [baud] [bw]; x stops", .func=wfrun_cmd},
    };
    for(unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        ESP_ERROR_CHECK(esp_console_cmd_register(&commands[i]));
}
