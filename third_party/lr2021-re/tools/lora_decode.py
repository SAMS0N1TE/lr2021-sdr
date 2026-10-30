# LoRa PHY decoder for LR2021 raw IQ captures (int8 interleaved I/Q).
# CLI:    lora_decode.py <iq8.bin> <fs_hz> [sf] [bw_hz]
# Module: decode(iq_complex, fs, sf=7, bw=62500) -> dict
# Bit-level steps follow EPFL gr-lora_sdr (Tapparel et al.): symbol-1 shift, /4 + reduced rate for the
# header block, Gray demap s^(s>>1), diagonal deinterleave, Hamming, header checksum, dewhitening, CRC-16.
import sys
import numpy as np
from scipy.signal import resample_poly
from fractions import Fraction
from meshcore_decode import parse_packet, MeshCoreError


class DecodeError(Exception):
    pass


def _gray(v): return v ^ (v >> 1)
def _bits(v, nb): return [(v >> (nb - 1 - i)) & 1 for i in range(nb)]
def _int(b): return int(''.join(map(str, b)), 2) if b else 0


def _deinterleave(block, sf_app, cw_len):
    inter = [_bits(v, sf_app) for v in block]
    de = [[0] * cw_len for _ in range(sf_app)]
    for i in range(cw_len):
        for j in range(sf_app):
            de[(i - j - 1) % sf_app][i] = inter[i][j]
    return [_int(r) for r in de]


def _hamming(cw, cr):
    c = _bits(cw, cr + 4)
    d = [c[3], c[2], c[1], c[0]]
    if cr >= 3 and not (cr == 4 and sum(c) % 2 == 0):
        s0 = c[0] ^ c[1] ^ c[2] ^ c[4]; s1 = c[1] ^ c[2] ^ c[3] ^ c[5]; s2 = c[0] ^ c[1] ^ c[3] ^ c[6]
        flip = {5: 3, 7: 2, 3: 1, 6: 0}.get(s0 + (s1 << 1) + (s2 << 2))
        if flip is not None:
            d[flip] ^= 1
    return _int(d)


def _header_ok(h):
    c4 = (h[0]>>3&1)^(h[0]>>2&1)^(h[0]>>1&1)^(h[0]&1)
    c3 = (h[0]>>3&1)^(h[1]>>3&1)^(h[1]>>2&1)^(h[1]>>1&1)^(h[2]&1)
    c2 = (h[0]>>2&1)^(h[1]>>3&1)^(h[1]&1)^(h[2]>>3&1)^(h[2]>>1&1)
    c1 = (h[0]>>1&1)^(h[1]>>2&1)^(h[1]&1)^(h[2]>>2&1)^(h[2]>>1&1)^(h[2]&1)
    c0 = (h[0]&1)^(h[1]>>1&1)^(h[2]>>3&1)^(h[2]>>2&1)^(h[2]>>1&1)^(h[2]&1)
    return ((c4 << 4) + (c3 << 3) + (c2 << 2) + (c1 << 1) + c0) == ((h[3] & 1) << 4) + h[4]


def _crc16(data):
    c = 0
    for byte in data:
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if (((c & 0x8000) >> 8) ^ (byte & 0x80)) else (c << 1)
            c &= 0xFFFF
            byte = (byte << 1) & 0xFF
    return c


def _whitening(n):
    out, s = [], 0xFF
    for _ in range(n):
        out.append(s)
        s = ((s << 1) & 0xFF) | (((s >> 7) ^ (s >> 5) ^ (s >> 4) ^ (s >> 3)) & 1)
    return out


def decode(x, fs, sf=7, bw=62500.0, *, preamble_start_s=None, return_phy=False):
    N = 1 << sf
    fr = Fraction(bw / fs).limit_denominator(64)
    x4 = resample_poly(np.asarray(x, dtype=np.complex64), 4 * fr.numerator, fr.denominator)
    x = x4[::4]  # one sample/chip for acquisition; retain quarter-chip timing phases
    n = np.arange(N)
    up = np.exp(1j * np.pi * (n * n / N - n))
    down = np.conj(up)
    L = len(x)

    def seg(arr, a):
        return arr[a:a + N] if 0 <= a and a + N <= len(arr) else None

    def mag(s, ref):
        return np.abs(np.fft.fft(s * ref))

    # 1) packet extent from the power envelope
    p = np.convolve(np.abs(x) ** 2, np.ones(N) / N, 'same')
    noise = np.percentile(p, 5)  # the packet can fill most of the window, so not the median
    thr = (noise + p.max()) / 2
    start = int(np.argmax(p > thr))
    end = L - int(np.argmax(p[::-1] > thr))
    if preamble_start_s is not None:
        # Continuous acquisition already located an upchirp train. Avoid
        # letting a leading partial packet or a later stronger packet choose
        # the power-envelope start for this candidate.
        start = max(0, int(round(preamble_start_s * bw)))
        end = L
    res = dict(duration_ms=(end - start) / bw * 1e3,
               snr_db=float(10 * np.log10(max(np.median(p[start:end]) - noise, 1e-9) / max(noise, 1e-9))))
    if end - start < 20 * N or start + N * 2 + N > L:
        raise DecodeError('no packet in capture')

    # 2) sync downchirps: the window with the strongest up-dechirp vs down-dechirp ratio.
    #    Consecutive identical chirps dechirp to a single tone for any window inside them.
    def ratio(w):
        s = seg(x, w)
        return 0 if s is None else mag(s, up).max() / (mag(s, down).max() + 1e-9)
    cand = range(start + 8 * N, min(end - N, start + 80 * N), N // 8)
    if len(cand) == 0:
        raise DecodeError('packet too short for sync')
    P_d = max(cand, key=ratio)
    if ratio(P_d) < 3:
        raise DecodeError('no sync downchirps found')

    carrier = np.exp(-2j * np.pi * np.arange(L) / N)  # one-bin phasor; carrier**cfo shifts by cfo bins

    def walk(xc, g):
        # count consecutive upchirp (preamble) symbols from g until the sync downchirps
        syms, i = [], 0
        while True:
            s = seg(xc, g + i * N)
            if s is None or i > 200:
                break
            if mag(s, up).max() > 2 * mag(s, down).max():
                break
            syms.append(int(np.argmax(mag(s, down))))
            i += 1
        return syms, i

    def try_candidate(g, u, sample_off):
        # Sync gating supplies an integer bin correction for this grid phase.
        # Correct the fractional CFO too: leaving it near a half-bin boundary
        # makes payload FFT decisions sensitive to fractional sample timing.
        cfo = u if u < N / 2 else u - N
        xc = x * carrier ** cfo
        syms, i = walk(xc, g)
        # Repeated preamble chirps measure fractional CFO independently of timing.
        # The integer correction still includes the selected grid's timing offset;
        # cfo_hz is the applied correction, not an absolute RF oscillator measurement.
        cfo_hz = cfo * bw / N
        if i >= 4:
            wins = [x[g + j * N:g + (j + 1) * N] for j in range(1, i - 1)]
            acc = sum(np.vdot(wins[m], wins[m + 1]) for m in range(len(wins) - 1))
            cfo_hz = (cfo + float(np.angle(acc) / (2 * np.pi))) * bw / N
        xc = x * np.exp(-2j * np.pi * np.arange(L) * (cfo_hz / bw))
        out = dict(res, cfo_hz=cfo_hz, sync=syms[-2:] if len(syms) >= 2 else syms,
                   preamble_syms=max(len(syms) - 2, 0), _i=i)
        # Retain fractional timing before decimation: interpolating the chip-rate signal
        # afterwards cannot recover the spectrum lost at the band edges.
        phase = int(round(sample_off * 4)) % 4
        if phase:
            xc = x4[phase::4] * np.exp(-2j * np.pi * np.arange(len(x4[phase::4])) * (cfo_hz / bw))
        data0 = g + i * N + int(2.25 * N) + int(np.floor(sample_off))
        raw, k = [], 0
        while seg(xc, data0 + k * N) is not None:
            raw.append(int(np.argmax(mag(seg(xc, data0 + k * N), down))))
            k += 1
        if len(raw) < 8:
            out['error'] = 'truncated before header'
            return out
        hs = [_gray(((v - 1) % N) // 4) for v in raw[:8]]
        hn = [_hamming(cw, 4) for cw in _deinterleave(hs, sf - 2, 8)]
        plen, has_crc, cr = (hn[0] << 4) + hn[1], hn[2] & 1, hn[2] >> 1
        out.update(len=plen, has_crc=bool(has_crc), cr=cr, header_ok=_header_ok(hn))
        if not out['header_ok'] or not 1 <= cr <= 4:
            out['error'] = 'bad header'
            return out
        nibbles, k = hn[5:], 8
        need = 2 * plen + (4 if has_crc else 0)
        while len(nibbles) < need and k + cr + 4 <= len(raw):
            blk = [_gray((v - 1) % N) for v in raw[k:k + cr + 4]]
            nibbles += [_hamming(cw, cr) for cw in _deinterleave(blk, sf, cr + 4)]
            k += cr + 4
        if len(nibbles) < need:
            out['error'] = f'truncated ({len(nibbles) // 2} of {plen} bytes in capture)'
            return out
        ws = _whitening(plen)
        out['payload'] = bytes(((nibbles[2*j+1] ^ (ws[j] >> 4)) << 4) | (nibbles[2*j] ^ (ws[j] & 0xF))
                               for j in range(plen))
        if has_crc and plen >= 2:
            rx = [(nibbles[2*plen + 2*j + 1] << 4) | nibbles[2*plen + 2*j] for j in range(2)]
            calc = _crc16(out['payload'][:-2]) ^ out['payload'][-1] ^ (out['payload'][-2] << 8)
            out['crc_ok'] = calc in (rx[0] | (rx[1] << 8), (rx[0] << 8) | rx[1])
        out.update(packet_start_s=g / bw, packet_end_s=(data0 + k * N) / bw)
        if return_phy:
            # Optional, JSON-safe receiver decisions for offline reconstruction.
            # Coordinates refer to the retained four-samples/chip grid.
            out['phy'] = dict(sf=sf, bw_hz=bw, grid_start_chips=g,
                              data_start_chips=data0 + phase / 4,
                              up_symbols=i, header_nibbles=hn[:5],
                              data_nibbles=nibbles, raw_symbols=raw[:k])
        if out.get('crc_ok') or not has_crc:
            try:
                out['meshcore'] = parse_packet(out['payload'])
            except MeshCoreError as e:
                out['meshcore_error'] = str(e)
        return out

    # Sweep the full symbol of grid phases. For each, rough-CFO from the preamble and cheaply read the two
    # symbols before the downchirps: the LoRa sync word is a fixed [8, 16] here, so phases that don't show it
    # are wrong boundaries. Full-decode only the gated phases and keep the first clean CRC.
    import os
    dbg = os.environ.get('LORA_DEBUG')
    SYNC = [8, 16]

    def rough_sync(g):
        us = [int(np.argmax(mag(seg(x, g + i * N), down))) for i in range(2, 6) if seg(x, g + i * N) is not None]
        if len(us) < 3:
            return None, None
        u = max(set(us), key=us.count)
        cfo = u if u < N / 2 else u - N
        xc = x * carrier ** cfo
        syms, i = walk(xc, g)
        return u, (syms[-2:] if len(syms) >= 2 else None)

    gated = []
    for g in range(max(start - N, 0), start + N):
        u, sw = rough_sync(g)
        if sw == SYNC:
            gated.append((g, u))
    # collapse each contiguous run of valid phases to its centre (one representative per band)
    reps = []
    if gated:
        run = [gated[0]]
        for gu in gated[1:]:
            if gu[0] - run[-1][0] <= 1:
                run.append(gu)
            else:
                reps.append(run[len(run) // 2])
                run = [gu]
        reps.append(run[len(run) // 2])

    best = None
    for g, u in reps:
        for off in (0, -1, 1, -2, 2, 0.25, 0.5, 0.75, -0.25, -0.5, -0.75):
            out = try_candidate(g, u, off)
            if dbg:
                print(f'  rep g={g} u={u} off={off:+g}: cfo={out.get("cfo_hz",0):.0f} '
                      f'hdr={out.get("header_ok")} crc={out.get("crc_ok")} err={out.get("error")} '
                      f'len={out.get("len")} sync={out.get("sync")}')
            if out.get('crc_ok'):
                return out
            rank = (0 if out.get('header_ok') else 1, out.get('error') is not None)
            if best is None or rank < best[0]:
                best = (rank, out)
    if best is None:
        raise DecodeError('no valid sync-word alignment found')
    return best[1]


def load_iq8(path):
    b = np.fromfile(path, dtype=np.int8).astype(np.float32)
    return b[0::2] + 1j * b[1::2]


if __name__ == '__main__':
    path, fs = sys.argv[1], float(sys.argv[2])
    sf = int(sys.argv[3]) if len(sys.argv) > 3 else 7
    bw = float(sys.argv[4]) if len(sys.argv) > 4 else 62500.0
    r = decode(load_iq8(path), fs, sf, bw)
    for key, v in r.items():
        if key == 'meshcore':
            import json
            print('meshcore: ' + json.dumps(v, indent=2, ensure_ascii=False))
        else:
            print(f'{key}: {v.hex(" ") if isinstance(v, bytes) else v}')
