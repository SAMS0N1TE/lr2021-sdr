#pragma once

// Included inside lr2021dbg after its shared 128 KiB ring is declared.
// Single cooperative owner: service the radio before every bounded USB write.
namespace iqstream {

enum : uint16_t { FIFO_LOSS = 1, QUEUE_FULL = 2, STOPPED = 4, DURATION = 8,
                  BUSY_TIMEOUT = 16, USB_STALL = 32, CONFIG_ERROR = 64 };
enum : uint8_t { START = 1, DATA = 2, END = 3 };
static uint32_t generation = 0;
static uint32_t crcTable[256];

#pragma pack(push, 1)
struct Header {
  char magic[4];
  uint8_t version, type;
  uint16_t headerBytes;
  uint32_t generation, sequence;
  uint64_t firstSample;
  uint32_t sampleCount, rawRate;
  uint16_t decimation, flags;
  uint32_t payloadBytes, payloadCrc, headerCrc;
};
struct Stats {
  uint64_t rawSamples, producedSamples, sentSamples, elapsedUs;
  uint32_t maxFifo, fifoFlags, maxServiceGapUs, maxQueue;
  uint32_t queueOverflows, busyTimeouts, spiHz;
  int32_t configError;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 48, "IQ stream header layout");
static_assert(sizeof(Stats) == 64, "IQ stream stats layout");

static void initCrc() {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int b = 0; b < 8; b++) c = (c >> 1) ^ ((c & 1) ? 0xEDB88320UL : 0);
    crcTable[i] = c;
  }
}
static uint32_t crcUpdate(uint32_t c, const uint8_t* data, uint32_t n) {
  while (n--) c = crcTable[(c ^ *data++) & 255] ^ (c >> 8);
  return c;
}
static uint32_t checksum(const void* data, uint32_t n) {
  return crcUpdate(0xFFFFFFFFUL, (const uint8_t*)data, n) ^ 0xFFFFFFFFUL;
}

static bool ready(Stats& stats) {
  uint32_t start = micros();
  while (digitalRead(P_LORA_BUSY)) {
    if ((uint32_t)(micros() - start) > 1000) { stats.busyTimeouts++; return false; }
  }
  return true;
}
static int16_t fastRxDelay = -1;
struct RxTimingGuard {
  uint32_t saved;
  RxTimingGuard() : saved(NRF_SPIM3->IFTIMING.RXDELAY) { if (fastRxDelay >= 0) NRF_SPIM3->IFTIMING.RXDELAY = fastRxDelay; }
  ~RxTimingGuard() { if (fastRxDelay >= 0) NRF_SPIM3->IFTIMING.RXDELAY = saved; }
};
static bool transfer(const uint8_t* tx, uint8_t* rx, uint32_t n, Stats& stats) {
  if (!ready(stats)) return false;
  RxTimingGuard timing;
  SPI.beginTransaction(fastSpi);
  digitalWrite(P_LORA_NSS, LOW);
  SPI.transfer(tx, rx, n);
  digitalWrite(P_LORA_NSS, HIGH);
  SPI.endTransaction();
  return true;
}
static bool read(uint16_t opcode, uint8_t* data, uint32_t n, Stats& stats) {
  uint8_t command[2] = { (uint8_t)(opcode >> 8), (uint8_t)opcode };
  uint8_t response[8] = {0}, nop[8] = {0};
  // The response transaction must clock a NOP (zero), not SPIM's default
  // all-ones over-read character, which the radio treats as an invalid opcode.
  if (n > 6 || !transfer(command, NULL, 2, stats) || !transfer(nop, response, n + 2, stats)) return false;
  memcpy(data, response + 2, n);
  return true;
}
static bool readFifo(uint8_t* data, uint32_t n, Stats& stats) {
  if (!ready(stats)) return false;
  uint8_t command[2] = {0, 1};
  RxTimingGuard timing;
  SPI.beginTransaction(fastSpi);
  digitalWrite(P_LORA_NSS, LOW);
  SPI.transfer(command, NULL, 2);
  SPI.transfer(NULL, data, n);
  digitalWrite(P_LORA_NSS, HIGH);
  SPI.endTransaction();
  return true;
}
static bool checkFlags(Stats& stats) {
  uint8_t flags[2] = {0};
  if (!read(0x011B, flags, 2, stats)) return false;
  stats.fifoFlags |= flags[0];
  // Only RX status bits 0..5 exist. Reserved bits indicate a bad SPI read.
  return !(flags[0] & 0xF0);
}

struct Exporter {
  Header header;
  const uint8_t* metadata;
  uint32_t headerOffset = 0, payloadOffset = 0, sequence = 0, readCount = 0;
  uint16_t decimation, pairBytes;
  uint32_t rawRate;
  uint64_t sentSamples = 0;
  bool active = false;

  Exporter(uint16_t dec, uint16_t bytes, uint32_t rate) : decimation(dec), pairBytes(bytes), rawRate(rate) { }
  void begin(uint8_t type, uint16_t flags, const void* info, uint32_t bytes) {
    metadata = (const uint8_t*)info;
    header = {{'L', 'R', 'I', 'Q'}, (uint8_t)(pairBytes == 4 ? 2 : 1), type, sizeof(Header), generation, sequence++,
              sentSamples, type == DATA ? bytes / pairBytes : 0, rawRate, decimation, flags, bytes, 0, 0};
    if (type == DATA) {
      uint32_t offset = readCount & (RING - 1);
      uint32_t first = min(bytes, RING - offset);
      uint32_t c = crcUpdate(0xFFFFFFFFUL, ring + offset, first);
      header.payloadCrc = crcUpdate(c, ring, bytes - first) ^ 0xFFFFFFFFUL;
    } else header.payloadCrc = checksum(metadata, bytes);
    header.headerCrc = checksum(&header, sizeof(Header) - 4);
    headerOffset = payloadOffset = 0;
    active = true;
  }
  // Never ask Stream::write to wait for space. Yield to FIFO service after <=64 bytes.
  bool pump(Stream& out) {
    if (!active) return false;
    int space = out.availableForWrite();
    if (space <= 0) return false;
    const uint8_t* data;
    uint32_t n = min((uint32_t)space, (uint32_t)64);
    bool writingHeader = headerOffset < sizeof(Header);
    if (writingHeader) {
      n = min(n, (uint32_t)sizeof(Header) - headerOffset);
      data = (const uint8_t*)&header + headerOffset;
    } else {
      n = min(n, header.payloadBytes - payloadOffset);
      if (header.type == DATA) {
        uint32_t offset = readCount & (RING - 1);
        n = min(n, RING - offset);
        data = ring + offset;
      } else data = metadata + payloadOffset;
    }
    uint32_t wrote = out.write(data, n);
    if (writingHeader) headerOffset += wrote;
    else {
      payloadOffset += wrote;
      if (header.type == DATA) readCount += wrote;
    }
    if (headerOffset == sizeof(Header) && payloadOffset == header.payloadBytes) {
      if (header.type == DATA) sentSamples += header.sampleCount;
      active = false;
      out.flush();
    }
    return wrote != 0;
  }
  bool finish(Stream& out, uint32_t timeoutMs = 2000) {
    uint32_t start = millis();
    while (active && (uint32_t)(millis() - start) < timeoutMs) { pump(out); yield(); }
    return !active;
  }
};

static void run(CustomLR2021& radio, Stream& out, uint32_t durationMs, uint32_t mhz, uint16_t dec, uint16_t bits = 8, uint32_t rate = 250000, uint16_t rxDelay = 0) {
  if ((mhz != 8 && mhz != 16 && mhz != 32) || dec < 1 || dec > 8) {
    out.println("iqstream error: SPI must be 8/16/32 MHz (32 experimental); decimation must be 1..8");
    return;
  }
  if ((bits != 8 && bits != 16) ||
      (bits == 8 && rate != 250000) ||
      (rate != 125000 && rate != 250000 && rate != 500000 && rate != 1000000)) {
    out.println("iqstream error: IQ8 requires 250000; IQ16 rates 125000/250000/500000/1000000");
    return;
  }
  if (rxDelay > 7 || (rxDelay && mhz != 32)) {
    out.println("iqstream error: RX delay 0..7 is only experimental at 32 MHz");
    return;
  }
  fastRxDelay = mhz == 32 ? (int16_t)rxDelay : -1;
  const uint16_t pairBytes = bits == 16 ? 4 : 2;
  hold = true;
  ringLen = 0; // the shared memory no longer contains a valid snapshot
  initCrc();
  generation++;
  Stats stats = {};
  stats.spiHz = mhz * 1000000UL;
  fastSpi = SPISettings(stats.spiHz, MSBFIRST, SPI_MODE0);
  Exporter tx(dec, pairBytes, rate);
  uint8_t stby = 0, lora = 0, raw = 0x0B, rx[3] = {255, 255, 255}, clear[2] = {0x3F, 0x3F};
  uint8_t route[11] = {255, 0, 0, 0, 0, 8, 0, 0, 0, 0, 1};
  if (bits == 16) route[5] = 0x0A; // native signed little-endian I16,Q16 (source 2)
  uint8_t mod[2] = {(uint8_t)(0x70 | (rate == 125000 ? 2 : rate == 250000 ? 3 : rate == 500000 ? 4 : 5)), 0x10};
  int16_t error = radio.rawCmd(0x0128, &stby, 1, NULL, 0);
  if (!error) error = radio.rawCmd(0x0207, &lora, 1, NULL, 0);
  if (!error) error = radio.rawCmd(0x0220, mod, sizeof(mod), NULL, 0);
  if (!error && bits == 8) error = radio.rawCmd(0x0207, &raw, 1, NULL, 0);
  if (!error) error = radio.rawCmd(0x0290, route, sizeof(route), NULL, 0);
  if (!error) error = radio.memWrite(0xF30028, 0x804400);
  if (!error) error = radio.memWrite(0xF30030, 0x3E8);
  if (!error) error = radio.rawCmd(0x0114, clear, 2, NULL, 0);
  stats.configError = error;
  tx.begin(START, error ? CONFIG_ERROR : 0, &stats, sizeof(stats));
  if (!tx.finish(out)) return;
  uint16_t reason = error ? CONFIG_ERROR : 0;
  if (!error) error = radio.rawCmd(0x020C, rx, sizeof(rx), NULL, 0);
  if (!error) error = radio.rawCmd(0x011E, NULL, 0, NULL, 0);
  if (!error) error = radio.rawCmd(0x0114, clear, 2, NULL, 0);
  if (error) { stats.configError = error; reason |= CONFIG_ERROR; }

  static uint8_t input[1020];
  uint32_t writeCount = 0, lastClock = micros(), lastService = lastClock;
  int32_t sumI = 0, sumQ = 0;
  uint16_t accumulated = 0;
  while (!reason) {
    uint32_t now = micros();
    stats.elapsedUs += (uint32_t)(now - lastClock);
    lastClock = now;
    stats.maxServiceGapUs = max(stats.maxServiceGapUs, (uint32_t)(now - lastService));
    lastService = now;
    if (durationMs && stats.elapsedUs >= (uint64_t)durationMs * 1000) { reason = DURATION; break; }
    // Consume binary-mode controls here, never dispatch arbitrary radio/mesh CLI commands.
    for (int i = 0; i < 32 && out.available(); i++) {
      if (out.read() == 3) reason = STOPPED;
    }
    if (reason) break;
    if (!checkFlags(stats)) { reason = stats.busyTimeouts ? BUSY_TIMEOUT : FIFO_LOSS; break; }
    uint8_t levelBytes[2];
    if (!read(0x011C, levelBytes, 2, stats)) { reason = BUSY_TIMEOUT; break; }
    uint32_t level = ((levelBytes[0] << 8) | levelBytes[1]) & ~3u;
    stats.maxFifo = max(stats.maxFifo, level);
    if (level >= 1024) { reason = FIFO_LOSS; break; }
    if (level) {
      uint32_t n = min(level, (uint32_t)sizeof(input));
      uint32_t outputBytes = ((accumulated + n / pairBytes) / dec) * pairBytes;
      if (outputBytes > RING - (writeCount - tx.readCount)) {
        stats.queueOverflows++;
        reason = QUEUE_FULL;
        break;
      }
      if (!readFifo(input, n, stats)) { reason = BUSY_TIMEOUT; break; }
      stats.rawSamples += n / pairBytes;
      uint32_t before = writeCount;
      for (uint32_t i = 0; i < n; i += pairBytes) {
        if (bits == 16) {
          sumI += (int16_t)((uint16_t)input[i] | ((uint16_t)input[i + 1] << 8));
          sumQ += (int16_t)((uint16_t)input[i + 2] | ((uint16_t)input[i + 3] << 8));
        } else {
          sumI += (int8_t)input[i]; sumQ += (int8_t)input[i + 1];
        }
        if (++accumulated == dec) {
          int32_t vi = sumI / dec, vq = sumQ / dec;
          ring[writeCount++ & (RING - 1)] = (uint8_t)vi;
          if (bits == 16) ring[writeCount++ & (RING - 1)] = (uint8_t)((uint16_t)vi >> 8);
          ring[writeCount++ & (RING - 1)] = (uint8_t)vq;
          if (bits == 16) ring[writeCount++ & (RING - 1)] = (uint8_t)((uint16_t)vq >> 8);
          sumI = sumQ = 0; accumulated = 0;
        }
      }
      if (!checkFlags(stats)) {
        writeCount = before; // never publish a chunk with observed FIFO loss
        reason = stats.busyTimeouts ? BUSY_TIMEOUT : FIFO_LOSS;
        break;
      }
      stats.producedSamples += (writeCount - before) / pairBytes;
      stats.maxQueue = max(stats.maxQueue, writeCount - tx.readCount);
    }
    if (!tx.active && writeCount - tx.readCount >= 1024) tx.begin(DATA, 0, NULL, 1024);
    // Service enough USB packets to exceed the input rate despite command/
    // DMA setup costs per producer iteration. Each write is nonblocking and
    // capped at 64 bytes; return after at most 256 bytes (IQ8) / 512 (IQ16).
    for (int i = 0; i < (bits == 16 ? 8 : 4) && tx.active; i++) tx.pump(out);
  }
  stats.elapsedUs += (uint32_t)(micros() - lastClock);
  error = radio.rawCmd(0x0128, &stby, 1, NULL, 0);
  if (error) { stats.configError = error; reason |= CONFIG_ERROR; }
  // Finish the current frame, then drain the verified queue after RX is stopped.
  if (!tx.finish(out)) return; // host detects missing END / partial frame
  while (writeCount != tx.readCount) {
    tx.begin(DATA, 0, NULL, min(writeCount - tx.readCount, (uint32_t)1024));
    if (!tx.finish(out)) return;
  }
  stats.sentSamples = tx.sentSamples;
  tx.begin(END, reason, &stats, sizeof(stats));
  tx.finish(out);
}

} // namespace iqstream
