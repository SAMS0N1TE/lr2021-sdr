#pragma once

// Debug serial commands for poking at LR2021 chip memory (ROM dump / AGC research).
// Enabled with -D LR2021_DEBUG_CLI. Not for production builds.
//
//   mr <addr> [words]          read words (hex addr), 8 per line
//   mw <addr> <value>          write one word
//   rc <op> <nread> [bytes..]  raw command: op hex, nread reply bytes, hex param bytes
//   romprobe                   read a few words at candidate ROM bases
//   err                        print + clear chip error register
//   stream <src> <x> <ms> <prekb> [mhz] [dec]  FIFO IQ streaming (0x0290 route from github.com/SAMS0N1TE/lr2021-sdr):
//                              fill a 128 KB ring on the nRF, trigger when a 4 KB block's power > x times the
//                              baseline, keep <prekb> KB before the trigger. 'streamdump' prints it (binary).
//                              Auto-gain reductions also trigger. x=0 disables triggering (timed capture).
//   streamstats                last capture's block power, actual/manual gain, raw int8 rail counts
//   iqstream <ms> [mhz] [dec] [bits] [rawrate] [rxdelay]   continuous LRIQ binary stream; 0 ms=until ETX (0x03).
//                              Default 16 MHz / decimation 3; leaves normal mesh held afterwards.
//   capwait <cfg> <x> <ms>     repeat one-shot raw captures (packet type 0x0B must be set) until the
//                              capture power is > x times the first capture's power, then dump it.
//   hold on|off                stop/resume the mesh loop (no radio access from MeshCore)
//   cap <cfg> <words> <ms> [trig]  RX capture via undocumented 0x0290 into RAM 0x804000:
//                              cfg = size(7:5)|mode(4:3)|src(2:0) byte, e.g. D0 = size 6, mode 2, src 0.
//                              Fills the buffer with A5A5A5A5, configures, enters RX (arms it), polls the write pointer (0x0291) for <ms>, stops the capture
//                              (0x0290 mode 0 gives the RAM bank back to the CPU), then dumps <words>.

#include <Arduino.h>
#include "CustomLR2021.h"
#include <SPI.h>

namespace lr2021dbg {

static bool hold = false;
static const uint32_t RING = 128 * 1024;
static uint8_t ring[RING];
static uint32_t ringLen = 0, ringStart = 0;
static char streamSummary[320];
// Readable after capture; no USB printing in the FIFO drain loop.
struct StreamGainSample { uint32_t power, gainReg, clipped; };
static StreamGainSample streamGain[512];
static uint32_t streamGainCount = 0;

static void printHex32(Stream& out, uint32_t v) {
  char buf[10];
  snprintf(buf, sizeof(buf), "%08lX", (unsigned long)v);
  out.print(buf);
}

// Read n words at addr in chunks of up to 32, print "AAAAAA: w0 w1 ..." lines of 8.
static void dump(CustomLR2021& r, Stream& out, uint32_t addr, uint32_t n) {
  uint32_t buf[32];
  while (n > 0) {
    uint8_t chunk = n > 32 ? 32 : n;
    int16_t st = r.memRead(addr, buf, chunk);
    for (uint8_t i = 0; i < chunk; i += 8) {
      char line[16];
      snprintf(line, sizeof(line), "%06lX:", (unsigned long)(addr + 4 * i));
      out.print(line);
      for (uint8_t j = i; j < chunk && j < i + 8; j++) { out.print(' '); printHex32(out, buf[j]); }
      if (st != RADIOLIB_ERR_NONE) { out.print(" !st="); out.print(st); }
      out.println();
    }
    addr += 4 * chunk;
    n -= chunk;
  }
}

// Direct SPI access for FIFO streaming: RadioLib's 2 MHz bus is too slow for ~500 KB/s.
static SPISettings fastSpi(8000000, MSBFIRST, SPI_MODE0);
static inline void waitBusy() { uint32_t t = micros(); while (digitalRead(P_LORA_BUSY) && micros() - t < 5000) {} }
static void spiXfer(const uint8_t* tx, uint8_t* rx, size_t n) {
  waitBusy();
  SPI.beginTransaction(fastSpi);
  digitalWrite(P_LORA_NSS, LOW);
  for (size_t i = 0; i < n; i++) { uint8_t b = SPI.transfer(tx ? tx[i] : 0); if (rx) rx[i] = b; }
  digitalWrite(P_LORA_NSS, HIGH);
  SPI.endTransaction();
}
// Command with response: write opcode, wait BUSY, then read status(2) + data.
static void fastRead(uint16_t op, uint8_t* data, size_t n) {
  uint8_t c[2] = { (uint8_t)(op >> 8), (uint8_t)op };
  spiXfer(c, NULL, 2);
  uint8_t rx[8] = { 0 };
  spiXfer(NULL, rx, 2 + n);
  memcpy(data, rx + 2, n);
}
static void readFifo(uint8_t* data, size_t n) {
  waitBusy();
  SPI.beginTransaction(fastSpi);
  digitalWrite(P_LORA_NSS, LOW);
  SPI.transfer(0x00); SPI.transfer(0x01);
  SPI.transfer(NULL, data, n);
  digitalWrite(P_LORA_NSS, HIGH);
  SPI.endTransaction();
}

#include "LR2021IQStream.h"

static bool handle(CustomLR2021& r, const char* cmd, Stream& out) {
  if (strncmp(cmd, "iqstream ", 9) == 0) {
    char* p;
    uint32_t ms = strtoul(cmd + 9, &p, 0);
    uint32_t mhz = strtoul(p, &p, 0);
    uint32_t dec = strtoul(p, &p, 0);
    uint32_t bits = strtoul(p, &p, 0);
    uint32_t rate = strtoul(p, &p, 0);
    uint32_t rxDelay = strtoul(p, &p, 0);
    iqstream::run(r, out, ms, mhz ? mhz : 16, dec ? dec : 3, bits ? bits : 8, rate ? rate : 250000, rxDelay);
    return true;
  }
  if (strcmp(cmd, "streamstats") == 0) {
    out.println("block power gain manual clipped");
    for (uint32_t i = 0; i < streamGainCount; i++) {
      const StreamGainSample& s = streamGain[i];
      out.printf("%lu %lu %lu %lu %lu\r\n", (unsigned long)(i + 1),
                 (unsigned long)s.power, (unsigned long)((s.gainReg >> 20) & 15),
                 (unsigned long)((s.gainReg >> 16) & 15), (unsigned long)s.clipped);
    }
    out.println("streamstats done");
    return true;
  }
  if (strncmp(cmd, "mr ", 3) == 0) {
    char* end;
    uint32_t addr = strtoul(cmd + 3, &end, 16);
    uint32_t n = strtoul(end, NULL, 0);
    if (n == 0) n = 1;
    dump(r, out, addr, n);
    out.println("mr done");
    return true;
  }
  if (strncmp(cmd, "mw ", 3) == 0) {
    char* end;
    uint32_t addr = strtoul(cmd + 3, &end, 16);
    uint32_t val = strtoul(end, NULL, 16);
    out.print("mw st="); out.println(r.memWrite(addr, val));
    return true;
  }
  if (strncmp(cmd, "rc ", 3) == 0) {
    char* p;
    uint16_t op = strtoul(cmd + 3, &p, 16);
    size_t nread = strtoul(p, &p, 0);
    uint8_t in[64], outb[128];
    size_t inLen = 0;
    while (*p && inLen < sizeof(in)) {
      char* q;
      unsigned long b = strtoul(p, &q, 16);
      if (q == p) break;
      in[inLen++] = (uint8_t)b;
      p = q;
    }
    if (nread > sizeof(outb)) nread = sizeof(outb);
    memset(outb, 0, sizeof(outb));
    int16_t st = r.rawCmd(op, in, inLen, outb, nread);
    out.print("rc st="); out.print(st);
    for (size_t i = 0; i < nread; i++) { char b[4]; snprintf(b, sizeof(b), " %02X", outb[i]); out.print(b); }
    out.println();
    return true;
  }
  if (strcmp(cmd, "hold on") == 0 || strcmp(cmd, "hold off") == 0) {
    hold = cmd[6] == 'n';
    out.print("hold st=0 "); out.println(hold ? "on" : "off");
    return true;
  }
  if (strncmp(cmd, "cap ", 4) == 0) {
    char* p;
    uint8_t cfg = strtoul(cmd + 4, &p, 16);
    uint32_t words = strtoul(p, &p, 0);
    uint32_t ms = strtoul(p, &p, 0);
    char* q;
    unsigned long trig = strtoul(p, &q, 16);
    if (q == p) trig = 0xFF;
    if (words > 2048) words = 2048;
    uint8_t ptr[2];
    uint8_t stby = 0;
    // Fill the buffer with a marker so fresh writes are visible.
    for (uint32_t i = 0; i < words; i++) r.memWrite(0x804000 + 4 * i, 0xA5A5A5A5);
    // Configure first, then enter RX: SetRx is what arms the capture DMA.
    r.rawCmd(0x0128, &stby, 1, NULL, 0);
    uint8_t c[10] = { (uint8_t)trig, 0, 0, 0, 0, cfg, 0, 0, (uint8_t)(words >> 8), (uint8_t)words };
    int16_t s1 = r.rawCmd(0x0290, c, sizeof(c), NULL, 0);
    uint8_t rx[3] = { 0xFF, 0xFF, 0xFF };
    int16_t s2 = r.rawCmd(0x020C, rx, sizeof(rx), NULL, 0);
    out.printf("cap cfg=%02X cfgst=%d rxst=%d ptr:", cfg, s1, s2);
    uint32_t t0 = millis();
    uint16_t last = 0xFFFF;
    uint32_t tc = 0;
    while (millis() - t0 < ms) {
      if (r.rawCmd(0x0291, NULL, 0, ptr, 2) != RADIOLIB_ERR_NONE) { out.print(" ERR"); break; }
      uint16_t v = (ptr[0] << 8) | ptr[1];
      if (v != last) { out.printf(" %lu:%04X", (unsigned long)(millis() - t0), v); last = v; tc = millis(); }
      if (tc && millis() - tc > 200) break;  // pointer moved, then settled: capture done
    }
    out.println();
    uint8_t stop[6] = { 0xFF, 0, 0, 0, 0, 0 };  // mode 0: restores bank mapping + default FIFO routing
    int16_t s3 = r.rawCmd(0x0290, stop, sizeof(stop), NULL, 0);
    int16_t s4 = r.rawCmd(0x0128, &stby, 1, NULL, 0);
    out.printf("cap stopst=%d stbyst=%d\r\n", s3, s4);
    dump(r, out, 0xF30044, 6);
    uint32_t fresh = 0, buf[32];
    for (uint32_t i = 0; i < words; i += 32) {
      uint8_t n = words - i > 32 ? 32 : words - i;
      r.memRead(0x804000 + 4 * i, buf, n);
      for (uint8_t j = 0; j < n; j++) fresh += buf[j] != 0xA5A5A5A5;
    }
    out.printf("cap fresh=%lu/%lu\r\n", (unsigned long)fresh, (unsigned long)words);
    if (fresh) dump(r, out, 0x804000, words);
    out.println("cap done");
    return true;
  }
  if (strcmp(cmd, "streamdump") == 0) {
    out.println(streamSummary);
    out.printf("STREAMBIN %lu\n", (unsigned long)ringLen);
    for (uint32_t i = 0; i < ringLen;) {
      uint32_t k = (ringStart + i) % RING;
      uint32_t n = ringLen - i;
      if (n > 512) n = 512;
      if (n > RING - k) n = RING - k;
      out.write(ring + k, n);
      i += n;
    }
    out.println();
    out.println("stream done");
    return true;
  }
  if (strncmp(cmd, "stream ", 7) == 0) {
    char* p;
    uint8_t src = strtoul(cmd + 7, &p, 0);
    uint32_t ratio = strtoul(p, &p, 0);
    uint32_t ms = strtoul(p, &p, 0);
    uint32_t pre = strtoul(p, &p, 0) * 1024;
    uint32_t mhz = strtoul(p, &p, 0);
    uint32_t dec = strtoul(p, &p, 0);   // N = average N IQ samples on the nRF (250k -> 250k/N S/s)
    if (dec < 1 || dec > 8) dec = 1;
    int32_t accI = 0, accQ = 0; uint32_t accN = 0;
    uint32_t rate = 0;
    fastSpi = SPISettings((mhz ? mhz : 8) * 1000000UL, MSBFIRST, SPI_MODE0);
    const uint32_t BLK = 4096;
    uint8_t stby = 0, rx[3] = { 0xFF, 0xFF, 0xFF }, clr[2] = { 0x3F, 0x3F };
    uint8_t route[11] = { 0xFF, (uint8_t)(rate >> 24), (uint8_t)(rate >> 16), (uint8_t)(rate >> 8), (uint8_t)rate,
                          (uint8_t)(0x08 | (src & 7)), 0, 0, 0, 0, 1 };
    r.rawCmd(0x0128, &stby, 1, NULL, 0);
    uint8_t raw = 0x0B;  // hidden packet type; with Semtech's PRAM patch loaded the FIFO route only runs here
    r.rawCmd(0x0207, &raw, 1, NULL, 0);
    int16_t s1 = r.rawCmd(0x0290, route, sizeof(route), NULL, 0);
    r.memWrite(0xF30028, 0x804400);  // documented 1 KB RX FIFO
    r.memWrite(0xF30030, 0x3E8);
    r.rawCmd(0x0114, clr, 2, NULL, 0);
    int16_t s2 = r.rawCmd(0x020C, rx, sizeof(rx), NULL, 0);
    r.rawCmd(0x011E, NULL, 0, NULL, 0);
    r.rawCmd(0x0114, clr, 2, NULL, 0);
    uint32_t w = 0, total = 0, blkpw = 0, blkn = 0, nblk = 0, maxlvl = 0, polls = 0;
    streamGainCount = 0;
    uint32_t clipped = 0;
    uint8_t baseGain = 0, triggerGain = 0;
    bool gainTrigger = false;
    uint64_t base = 0;
    int32_t stopAt = -1;
    bool hit = false;
    uint8_t lv[2], fl[2] = { 0, 0 };
    uint32_t t0 = millis();
    while (millis() - t0 < ms) {
      if ((polls & 31) == 0) yield();  // keep USB alive on long runs
      fastRead(0x011C, lv, 2); polls++;
      uint32_t level = ((lv[0] << 8) | lv[1]) & ~3u;
      if (level > maxlvl) maxlvl = level;
      while (level) {
        static uint8_t tmp[1020];
        uint32_t n = level > 1020 ? 1020 : level;
        readFifo(tmp, n);
        level -= n;
        uint32_t m = 0;  // bytes to store, written back into tmp
        uint8_t* o = tmp;
        for (uint32_t i = 0; i < n; i += 2) {
          if ((int8_t)tmp[i] <= -127 || (int8_t)tmp[i] >= 126) clipped++;
          if ((int8_t)tmp[i + 1] <= -127 || (int8_t)tmp[i + 1] >= 126) clipped++;
          accI += (int8_t)tmp[i]; accQ += (int8_t)tmp[i + 1];
          if (++accN == dec) {
            tmp[m++] = (uint8_t)(int8_t)(accI / (int32_t)dec);
            tmp[m++] = (uint8_t)(int8_t)(accQ / (int32_t)dec);
            accI = accQ = 0; accN = 0;
          }
        }
        for (uint32_t i = 0; i < m; i++) {
          int8_t v = (int8_t)o[i];
          ring[w] = o[i];
          w = (w + 1) % RING;
          blkpw += v * v;
          if (++blkn == BLK) {
            nblk++;
            uint32_t gainReg = 0;
            bool gainValid = r.memRead(0xF30154, &gainReg, 1) == RADIOLIB_ERR_NONE;
            uint8_t gain = (gainReg >> 20) & 15;
            bool automatic = ((gainReg >> 16) & 15) == 0;
            gainValid = gainValid && gain >= 1 && gain <= 13;
            if (streamGainCount < 512) {
              streamGain[streamGainCount++] = { blkpw, gainReg, clipped };
            }
            clipped = 0;
            if (nblk > 4 && nblk <= 36) {
              base += blkpw;
              if (gainValid && automatic && gain > baseGain) baseGain = gain;
            } else if (nblk > 36 && ratio != 0 && !hit) {
              // AGC holds output power roughly constant for strong packets.
              // Its gain reduction is evidence of a signal even when IQ power
              // never reaches the fixed multiple of the quiet baseline.
              // Keep learning the quiet (highest) gain. A packet during baseline
              // learning must not pin the reference at its low gain forever.
              if (gainValid && automatic && gain > baseGain) baseGain = gain;
              bool gainDrop = gainValid && automatic && baseGain > 0 && gain < baseGain;
              if (gainDrop || (uint64_t)blkpw * 32 > base * ratio) {
                hit = true;
                gainTrigger = gainDrop;
                triggerGain = gain;
                stopAt = total + i + 1 + (RING - pre);
              }
            }
            blkpw = 0; blkn = 0;
          }
        }
        total += m;
      }
      if (hit && (int32_t)total >= stopAt) break;
    }
    uint32_t el = millis() - t0;
    fastRead(0x011B, fl, 2);
    r.rawCmd(0x0128, &stby, 1, NULL, 0);
    uint32_t len = total < RING ? total : RING;
    uint32_t start = total < RING ? 0 : w;
    snprintf(streamSummary, sizeof(streamSummary),
             "stream src=%u dec=%lu routest=%d rxst=%d bytes=%lu ms=%lu Bps=%lu maxlvl=%lu polls=%lu fifoflags=%02X hit=%d base=%lu gainbase=%u trigger=%s gain=%u",
             src, (unsigned long)dec, s1, s2, (unsigned long)total, (unsigned long)el,
             (unsigned long)(el ? (uint64_t)total * 1000 / el : 0), (unsigned long)maxlvl, (unsigned long)polls, fl[0], hit,
             (unsigned long)(base / 32), baseGain,
             hit ? (gainTrigger ? "gain" : "power") : "none", triggerGain);
    ringLen = len; ringStart = start;
    out.println("stream done");
    return true;
  }
  if (strncmp(cmd, "capwait ", 8) == 0) {
    char* p;
    uint8_t cfg = strtoul(cmd + 8, &p, 16);
    uint32_t ratio = strtoul(p, &p, 0);
    uint32_t ms = strtoul(p, &p, 0);
    const uint32_t words = 2048;
    static uint32_t buf[words];
    uint8_t stby = 0, rx[3] = { 0xFF, 0xFF, 0xFF }, stop[6] = { 0xFF, 0, 0, 0, 0, 0 }, ptr[2];
    uint8_t c[10] = { 0xFF, 0, 0, 0, 0, cfg, 0, 0, (uint8_t)(words >> 8), (uint8_t)words };
    uint64_t base = 0;
    uint32_t t0 = millis(), n = 0;
    while (millis() - t0 < ms) {
      r.rawCmd(0x0128, &stby, 1, NULL, 0);
      r.rawCmd(0x0290, c, sizeof(c), NULL, 0);
      r.rawCmd(0x020C, rx, sizeof(rx), NULL, 0);
      uint32_t tw = millis();
      while (millis() - tw < 40) {  // one-shot fill takes ~17 ms
        if (r.rawCmd(0x0291, NULL, 0, ptr, 2) != RADIOLIB_ERR_NONE) break;
        if (((ptr[0] << 8) | ptr[1]) >= 0x2000) break;
      }
      r.rawCmd(0x0290, stop, sizeof(stop), NULL, 0);
      r.rawCmd(0x0128, &stby, 1, NULL, 0);
      for (uint32_t i = 0; i < words; i += 32) r.memRead(0x804000 + 4 * i, &buf[i], 32);
      uint64_t pw = 0;
      for (uint32_t i = 0; i < words; i++) {
        for (int b = 0; b < 4; b++) { int8_t v = (int8_t)(buf[i] >> (8 * b)); pw += v * v; }
      }
      n++;
      out.printf("capwait n=%lu t=%lu pw=%lu ptr=%02X%02X\r\n", (unsigned long)n, (unsigned long)(millis() - t0),
                 (unsigned long)pw, ptr[0], ptr[1]);
      if (n == 1) { base = pw ? pw : 1; out.printf("capwait base=%lu\r\n", (unsigned long)base); continue; }
      if (pw > base * ratio) {
        out.printf("capwait hit n=%lu t=%lu pw=%lu (%lux)\r\n", (unsigned long)n, (unsigned long)(millis() - t0),
                   (unsigned long)pw, (unsigned long)(pw / base));
        for (uint32_t i = 0; i < words; i += 8) {
          out.printf("%06lX:", (unsigned long)(0x804000 + 4 * i));
          for (int j = 0; j < 8; j++) out.printf(" %08lX", (unsigned long)buf[i + j]);
          out.print("\r\n");
        }
        out.println("capwait done");
        return true;
      }
    }
    out.printf("capwait timeout n=%lu\r\n", (unsigned long)n);
    out.println("capwait done");
    return true;
  }
  if (strcmp(cmd, "err") == 0) {
    uint16_t e = 0;
    int16_t st = r.getChipErrors(&e);
    out.print("err st="); out.print(st); out.print(" errors=0x"); out.println(e, HEX);
    r.clearChipErrors();
    return true;
  }
  if (strcmp(cmd, "romprobe") == 0) {
    static const uint32_t bases[] = {
      0x000000, 0x100000, 0x200000, 0x400000, 0x600000, 0x700000, 0x800000 - 0x101000 + 0x1000,
      0x900000, 0xA00000, 0xC00000, 0xE00000, 0xF00000, 0xF80000,
    };
    static const uint32_t offs[] = { 0x0000, 0x24E0, 0xB3A4 };
    for (uint32_t b : bases) {
      for (uint32_t o : offs) {
        if (b + o > 0xFFFFFF) continue;
        dump(r, out, b + o, 4);
      }
    }
    out.println("romprobe done");
    return true;
  }
  return false;
}

}  // namespace lr2021dbg
