// On-chip spectrum: windowed 256-point FFTs over a raw capture, power averaged per bin.
//
//   entry 7 (fft_cmd):    FFT of a capture already in RAM (host ran it), u64 power sums at ACC.
//   entry 9 (capfft_cmd): capture + FFT + dB in one command: 256 x u8 (0.5 dB steps) at DBO;
//                         optionally overlapped with the next capture.
//
// The capture (source 0, int8 I/Q, 250 kS/s with the 62.5 kHz LoRa setting) goes to 0x804000. The
// capture DMA runs while this code holds the CPU, so the ROM's own handlers can drive it.
// The chip must be in packet type 0x0B (raw) and not receiving LoRa (0x804000 is then free).
//
// FFT: 16-bit block floating point (s16 work arrays, s32 arithmetic, no 64-bit multiplies): a
// stage scales by 1/2 only when it could overflow, and the exponent is folded back into a u64
// power sum. The first two stages need no multiplies (twiddles 1 and -j).
//
// Build (build.sh): -mcpu=em4_dmips -mrf16 (16-entry register file), -mno-sdata (gp belongs to the
// ROM), -fcall-saved-blink with the patched GCC, -mtune=arc700 -fschedule-insns.
#include "fft_tables.h"

typedef unsigned int u32;
typedef int s32;
typedef unsigned long long u64;
typedef short s16;
typedef unsigned char u8;
typedef signed char s8;

#define N 256
#define CAP ((const s8 *)0x804000)          // up to 5 KB
#define ACC ((u64 *)0x805400)               // 256 x u64 power (entry 7)
// Outside the capture bank (0x804000-0x805FFF is the DMA's while a capture runs), so the FFT and
// the host's readback can run while the next capture fills it:
#define DBO ((u8 *)0x807B00)                // entry 9 result: 256 x u8 half-dB
#define W ((s16 *)0x807C00)                 // work: 256 x {s16 re, s16 im}, 1 KB, up to 0x808000
// Twiddles {cos, -sin}(2 pi k / 256), k = 0..127, Q15, built in RAM from the quarter-wave table QC:
// 0x807800-0x807FFF sits above the stack (top 0x8077F8) and nothing was seen writing it.
#define TWR ((s16 *)0x807800)               // 512 B
#define TW_MARK ((volatile u32 *)0x807A00)
#define TW_MAGIC 0x54574931                 // "TWI1"

// One radix-2 DIT stage (stages 3-8), in place on w. For k = 0..half-1 (twiddle tw[k * 128/half])
// and i = k, k + 2*half, ...:   t = b * w >> 15;  a' = (a + t) >> SH;  b' = (a - t) >> SH
// (a = w[i], b = w[i + half]). SH is a constant, so the unscaled and scaled variants get their own
// loops: 22 and 26 instructions in zero-overhead loops, no stack traffic. That needs the patched
// GCC (docker/gcc-arc-blink.patch, -fcall-saved-blink: blink becomes a 13th register) and
// -mtune=arc700 -fschedule-insns, whose pipeline model issues the multiplies before the a loads.
static inline __attribute__((always_inline))
void stage_k(s16 *w, u32 half, const s16 *tw, u32 cnt, const u32 SH) {
  u32 stride = 4 * half;                    // s16 units: 2 * half points
  for (u32 k = 0; k < half; k++, tw += 2 * cnt) {
    s32 wr = tw[0], wi = tw[1];
    s16 *pa = w + 2 * k, *pb = pa + 2 * half;
    for (u32 j = 0; j < cnt; j++, pa += stride, pb += stride) {
      s32 br = pb[0], bi = pb[1];
      s32 tr = (br * wr - bi * wi) >> 15;
      s32 ti = (br * wi + bi * wr) >> 15;
      s32 a0 = pa[0], a1 = pa[1];
      pa[0] = (a0 + tr) >> SH; pa[1] = (a1 + ti) >> SH;
      pb[0] = (a0 - tr) >> SH; pb[1] = (a1 - ti) >> SH;
    }
  }
}
// tw: 128 x {cos, -sin} Q15; count of butterflies per twiddle = 128 / half (half is a power of 2).
__attribute__((optimize("O2"), noinline))
static void fft_stage(s16 *w, u32 half, u32 sh, const s16 *tw) {
  u32 cnt = 128u >> __builtin_ctz(half);
  if (sh) stage_k(w, half, tw, cnt, 1); else stage_k(w, half, tw, cnt, 0);
}
#define MAXSEG 10

struct resp { u32 len; u8 *buf; };

typedef int (*handler_t)(u32 len, const u8 *p, struct resp *r);
#define H_SET_RX      ((handler_t)0xFCFC)
#define H_SET_FS      ((handler_t)0xEB40)
#define H_CAPTURE_CFG ((handler_t)0xF9A0)   // 0x0290
#define H_CAPTURE_PTR ((handler_t)0x5B44)   // 0x0291: 2-byte write pointer
#define POLL_MAX 200000

static inline u32 bitrev8(u32 x) {
  x = ((x & 0xF0) >> 4) | ((x & 0x0F) << 4);
  x = ((x & 0xCC) >> 2) | ((x & 0x33) << 2);
  return ((x & 0xAA) >> 1) | ((x & 0x55) << 1);
}

static inline u32 absv(s32 v) { return v < 0 ? -v : v; }

// 64-bit shifts by a variable amount without libgcc (__ashldi3 / __lshrdi3 don't exist here).
static inline u64 shl64(u32 v, u32 s) {            // s < 32
  if (s == 0) return v;
  return ((u64)(v >> (32 - s)) << 32) | (u32)(v << s);
}
static inline u32 bits_at(u64 p, u32 s) {          // (u32)(p >> s), s < 64
  u32 lo = (u32)p, hi = (u32)(p >> 32);
  if (s == 0) return lo;
  if (s >= 32) return hi >> (s - 32);
  return (lo >> s) | (hi << (32 - s));
}

// One windowed 256-point FFT of segment x (int8 I/Q pairs) into W, block floating point.
// Returns e: the true DFT of the windowed input is W << e.
//
// Bounds: the windowed input has components <= 2^14, so magnitudes <= 2^14.5. A radix-2 stage at
// most doubles the magnitude. The scaling is planned once from the input's largest component m0:
// stages run unscaled while the worst-case bound stays <= 8192 per component, then scale by 1/2.
// Magnitudes therefore stay <= 2^14.5 = 23170: they fit s16, and each twiddle product
// (<= 2^14.5 * 2^15) and their sum (<= 2^30.5) fits s32. No per-butterfly bookkeeping.
static u32 bench_mode __attribute__((section(".data"))) = 0;   // in the image (.bss is not uploaded)

static void tw_init(void) {
  for (u32 k = 0; k < 128; k++) {
    s32 c = k <= 64 ? QC[k] : -QC[128 - k];
    s32 s = k <= 64 ? QC[64 - k] : QC[k - 64];
    TWR[2 * k] = c; TWR[2 * k + 1] = -s;
  }
  *TW_MARK = TW_MAGIC;
}

static inline s32 hann(u32 n) {                     // Q15, from the twiddle cosines, symmetric
  u32 nn = n < 128 ? n : 256 - n;
  return nn < 128 ? (32767 - TWR[2 * nn]) >> 1 : 32767;
}

// One windowed 256-point FFT of segment x (int8 I/Q pairs) into W, block floating point.
// Returns e: the true DFT of the windowed input is W << e.
//
// Bounds: windowed components are <= 2^14 (magnitude <= 2^14.5); a radix-2 stage at most doubles
// the magnitude. The scaling is planned from the input's largest raw sample: stages run unscaled
// while the worst-case component bound stays <= 8192, then scale by 1/2, so magnitudes stay
// <= 2^14.5 = 23170 (fits s16; twiddle products and their sums fit s32).
//
// Stages 1-2 are fused with the windowed, bit-reversed load: with bit-reversed input, output
// positions i..i+3 (i = bitrev(r), r < 64) of stage 2 come from samples r, r+128, r+64, r+192,
// so each of 64 iterations is one multiply-free radix-4 butterfly. Stages 3-8: fft_stage.
// Front half: the max pre-pass and the fused load (the only parts that read the capture x).
// Fills the scaling plan sh[8]; returns e.
__attribute__((optimize("O2"))) static u32 fft_front(const s8 *x, u32 *sh) {
  s16 *w = W;
  // largest raw sample magnitude -> windowed bound (window <= 1): (|x| * 32767) >> 8
  u32 mx = 0;
  for (u32 n = 0; n < 2 * N; n++) { s32 v = x[n]; mx |= absv(v); }
  u32 m = (mx * 32767) >> 8;
  u32 e = 0;
  for (u32 st = 0; st < 8; st++) {
    if (m > 8192) { sh[st] = 1; e++; } else { sh[st] = 0; m <<= 1; }
  }
  if (bench_mode == 1) return 0;
  u32 s1 = sh[0], s2 = sh[1];
  for (u32 r = 0; r < 64; r++) {
    s32 h0 = hann(r), h1 = hann(r + 128), h2 = hann(r + 64), h3 = hann(r + 192);
    s32 r0 = (x[2 * r] * h0) >> 8,         i0 = (x[2 * r + 1] * h0) >> 8;
    s32 r1 = (x[2 * (r + 128)] * h1) >> 8, i1 = (x[2 * (r + 128) + 1] * h1) >> 8;
    s32 r2 = (x[2 * (r + 64)] * h2) >> 8,  i2 = (x[2 * (r + 64) + 1] * h2) >> 8;
    s32 r3 = (x[2 * (r + 192)] * h3) >> 8, i3 = (x[2 * (r + 192) + 1] * h3) >> 8;
    if (bench_mode == 2) continue;
    // stage 1: pairs (0,1) and (2,3); stage 2: (a0, a2) and (a1, a3 * -j)
    s32 a0r = (r0 + r1) >> s1, a0i = (i0 + i1) >> s1, a1r = (r0 - r1) >> s1, a1i = (i0 - i1) >> s1;
    s32 a2r = (r2 + r3) >> s1, a2i = (i2 + i3) >> s1, a3r = (r2 - r3) >> s1, a3i = (i2 - i3) >> s1;
    s16 *o = w + 2 * bitrev8(r);
    o[0] = (a0r + a2r) >> s2; o[1] = (a0i + a2i) >> s2;
    o[4] = (a0r - a2r) >> s2; o[5] = (a0i - a2i) >> s2;
    o[2] = (a1r + a3i) >> s2; o[3] = (a1i - a3r) >> s2;      // a3 * -j = (a3i, -a3r)
    o[6] = (a1r - a3i) >> s2; o[7] = (a1i + a3r) >> s2;
  }
  return e;
}
// Back half: stages 3-8 on W (no access to the capture).
static void fft_back(const u32 *sh) {
  for (u32 half = 4, st = 2; half < N; half <<= 1, st++) fft_stage(W, half, sh[st], TWR);
}
static u32 fft256(const s8 *x) {
  u32 sh[8], e = fft_front(x, sh);
  if (bench_mode != 1 && bench_mode != 2) fft_back(sh);
  return e;
}

// Sum power over segments [skip, skip+nseg) into ACC (u64 per bin, unscaled DFT power / 2^... see
// fft256: (re^2 + im^2) << 2e).
__attribute__((optimize("O2"))) static void spectrum(u32 nseg, u32 skip) {
  u64 *acc = ACC;
  if (*TW_MARK != TW_MAGIC) tw_init();               // first use, or something overwrote it
  for (u32 k = 0; k < N; k++) acc[k] = 0;
  for (u32 seg = skip; seg < skip + nseg; seg++) {
    u32 e2 = 2 * fft256(CAP + seg * N * 2);
    const s16 *w = W;
    for (u32 k = 0; k < N; k++) {
      s32 r = w[2 * k], i = w[2 * k + 1];
      acc[k] += shl64((u32)(r * r + i * i), e2);      // <= 2^30 << 16 per segment
    }
  }
}

// 0.5 dB steps: half-dB = 2 * 10*log10(p) = 6.0206 * log2(p); log2 from the leading-zero count +
// a table. Inputs up to 2^50 give at most ~301 half-dB: offset by OFF so the useful range fits u8.
#define DB_OFF 40                                     // 20 dB
static u8 half_db(u64 p) {
  if (p == 0) return 0;
  u32 hi = p >> 32, n, top;
  if (hi) { n = 63 - __builtin_clz(hi); }
  else { n = 31 - __builtin_clz((u32)p); }
  top = n >= 5 ? bits_at(p, n - 5) & 31 : ((u32)p << (5 - n)) & 31;
  u32 l2 = (n << 8) + LOG2F[top];                     // log2(p) in Q8
  s32 v = (s32)((l2 * 1541) >> 16) - DB_OFF;          // 1541/65536 = 6.0206/256
  return v < 0 ? 0 : v > 255 ? 255 : (u8)v;
}

// Half-dB of a 32-bit power: 6.0206 * log2(p) - DB_OFF.
static inline u8 half_db32(u32 p, u32 e2) {        // e2 = 2e: power scale 4^e adds 6.0206 * 2e
  if (p == 0) return 0;
  u32 n = 31 - __builtin_clz(p);
  u32 top = n >= 5 ? (p >> (n - 5)) & 31 : (p << (5 - n)) & 31;
  u32 l2 = ((n + e2) << 8) + LOG2F[top];
  s32 v = (s32)((l2 * 1541) >> 16) - DB_OFF;
  return v < 0 ? 0 : v > 255 ? 255 : (u8)v;
}

// One-shot capture of `words` 32-bit words (128 per segment) into 0x804000, in two halves so the
// CPU can work while the DMA fills the bank: cap_start arms it and starts RX; cap_finish polls the
// write pointer, stops the capture and goes to FS, and returns the final pointer. FS rather than
// standby: SetRx from FS takes 0.19 ms instead of 1.27 ms (synthesizer stays locked).
static const u8 rx_cont[3] = { 0xFF, 0xFF, 0xFF };
static void cap_start(u32 words) {
  u8 cfg[10];
  cfg[0] = 0xFF; cfg[1] = 0; cfg[2] = 0; cfg[3] = 0; cfg[4] = 0; cfg[5] = 0xD0; cfg[6] = 0; cfg[7] = 0;
  cfg[8] = (u8)(words >> 8); cfg[9] = (u8)words;
  H_CAPTURE_CFG(10, cfg, 0);
  H_SET_RX(3, rx_cont, 0);
}
static u32 cap_finish(u32 words) {
  static const u8 stop[6] = { 0xFF, 0, 0, 0, 0, 0 };
  u8 ptr[2];
  struct resp pr;
  pr.len = 0; pr.buf = ptr;
  u32 v = 0;
  for (u32 n = 0; n < POLL_MAX; n++) {
    H_CAPTURE_PTR(0, 0, &pr);
    v = ((u32)ptr[0] << 8) | ptr[1];
    if (v >= words * 4) break;
  }
  H_CAPTURE_CFG(6, stop, 0);
  H_SET_FS(0, 0, 0);
  return v;
}
// Set while a capture started by an overlapped entry-9 call is still running (bank DMA-owned).
static u32 armed __attribute__((section(".data"))) = 0;
// Entry 7. Params: nseg, skip. 256 x u64 power at ACC. Returns 2 bytes: segments processed.
int fft_cmd(u32 len, const u8 *p, struct resp *r) {
  if (len < 2) return 1;
  u32 nseg = p[0], skip = p[1];
  if (nseg == 0 || nseg + skip > MAXSEG) return 1;
  spectrum(nseg, skip);
  r->buf[0] = 0; r->buf[1] = (u8)nseg; r->len = 2;
  return 3;
}

// Entry 9. Params: nseg (1..10) [, overlap]. Capture nseg segments, average their spectra, write
// 256 x u8 half-dB values at DBO (natural FFT order: bin 0 = DC, 128..255 = negative frequencies).
// Returns 4 bytes: segments (u16), capture pointer (u16); 0 segments = capture didn't complete.
//
// overlap (nseg 1 only): right after the windowed load has copied the samples into W, start the
// next capture and leave it running; stages 3-8, the dB conversion, the host's readback and the
// next command's setup all happen while it fills. The next call then usually finds it complete.
// The capture bank stays DMA-owned in between: nseg 0 stops it (do that before anything else
// touches 0x804000-0x805FFF, e.g. entry 7 or a host capture).
int capfft_cmd(u32 len, const u8 *p, struct resp *r) {
  if (len < 1) return 1;
  u32 nseg = p[0], ov = len > 1 ? p[1] : 0;
  if (nseg > MAXSEG || (ov && nseg != 1)) return 1;
  u32 v = 0, had = armed;
  if (had) { armed = 0; v = cap_finish(N * 2 / 4); }   // started by the previous overlapped call
  if (nseg == 0) return 2;
  if (!ov || !had) { cap_start(nseg * (N * 2 / 4)); v = cap_finish(nseg * (N * 2 / 4)); }
  u32 done = 0;
  if (v >= nseg * N * 2) {
    u8 *db = DBO;
    if (*TW_MARK != TW_MAGIC) tw_init();
    if (nseg == 1) {                                 // fast path: dB straight from the 32-bit power
      u32 sh[8], e2 = 2 * fft_front(CAP, sh);
      if (ov) { cap_start(N * 2 / 4); armed = 1; }   // the capture is free again: refill it now
      fft_back(sh);
      const s16 *w = W;
      for (u32 k = 0; k < N; k++) {
        s32 re = w[2 * k], im = w[2 * k + 1];
        db[k] = half_db32((u32)(re * re + im * im), e2);
      }
    } else {
      spectrum(nseg, 0);
      const u64 *acc = ACC;
      for (u32 k = 0; k < N; k++) db[k] = half_db(acc[k]);
    }
    done = nseg;
  }
  r->buf[0] = done >> 8; r->buf[1] = done; r->buf[2] = v >> 8; r->buf[3] = v; r->len = 4;
  return 3;
}

// Entry 10 (benchmark). Param: mode. 0: 1,000,000 iterations of a one-NOP zero-overhead loop
// (1 cycle each, so the run time gives the CPU clock). 1: max pre-pass x10. 2: + windowed loads
// x10. 3 (unused) / 4: full FFT x10. Time it with the console's rc us=.
void bench_nops(void);   // bench.S
void bench_micro(u32 mode);   // bench.S
int bench_cmd(u32 len, const u8 *p, struct resp *r) {
  if (len < 1) return 1;
  u32 mode = p[0];
  if (mode == 0) { bench_nops(); return 2; }
  if (mode >= 16) { bench_micro(mode); return 2; }
  if (*TW_MARK != TW_MAGIC) tw_init();
  bench_mode = mode == 4 ? 0 : mode;
  for (u32 i = 0; i < 10; i++) fft256(CAP);
  bench_mode = 0;
  return 2;
}
