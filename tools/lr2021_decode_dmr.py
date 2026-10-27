#!/usr/bin/env python3
"""Demodulate and decode DMR bursts from signed I8/Q8 playback files.

Software loopback for lr2021_make_dmr.py output: FM discriminator, RRC alpha
0.2 matched filter, sync search, 4-level slicing, then Golay(20,8) Slot Type,
BPTC(196,96) with Hamming correction, and the RS(12,9), CRC-16 or CRC-32 check
for each burst. No expected bits or identifiers are given to the decoder.
Not an RF receiver: it reads the generated buffer, not a capture.
Requires NumPy.
"""
import argparse
import json
from pathlib import Path
import wave

import numpy as np

from lr2021_make_dmr import (CRC_MASK, CSBK, DATA_HEADER, DATA_TYPES, DEVIATION_HZ, H13_9,
    H15_11, LC_MASK, RATE_HALF_DATA, SPAN, SYMBOL_RATE, SYNC, bits_bytes, byte_bits,
    crc32_dmr, crc_ccitt, int_bits, parity, rrc, rs129_parity, slot_type)

STEP = 10                                  # timing search grid, per symbol
SYNC_START = 54                            # sync is burst symbols 54..77
SLOT_WORDS = {slot_type(cc, dt): (cc, dt) for cc in range(16) for dt in range(16)}


def iq_from_bytes(data):
    if not data or len(data) % 2:
        raise ValueError('Expected interleaved signed I8/Q8')
    pairs = np.frombuffer(data, np.int8).reshape(-1, 2).astype(float)
    return pairs[:, 0] + 1j * pairs[:, 1]


def discriminator(iq, rate):
    """Instantaneous frequency in Hz; zero where the carrier is off."""
    freq = np.zeros(len(iq))
    freq[1:] = np.angle(iq[1:] * iq[:-1].conj()) * rate / (2 * np.pi)
    level = np.abs(iq)
    on = np.minimum(level, np.r_[level[0], level[:-1]]) > .25 * level.max()
    return freq * on


def matched_filter(freq, rate):
    sps = rate / SYMBOL_RATE
    half = int(SPAN * sps)
    taps = rrc(np.arange(-half, half + 1) / sps)
    return np.convolve(freq, taps / taps.sum(), 'same')


def hamming_table(taps, k):
    n = k + len(taps)
    table = {}
    for pos in range(n):
        word = [0] * n
        word[pos] = 1
        table[tuple(syndrome(word, taps, k))] = pos
    return table


def syndrome(word, taps, k):
    return [p ^ word[k + i] for i, p in enumerate(parity(word[:k], taps))]


ROW_TABLE, COLUMN_TABLE = hamming_table(H15_11, 11), hamming_table(H13_9, 9)


def correct(word, taps, k, table):
    """Single-error Hamming correction; None when the syndrome is not a single error."""
    s = tuple(syndrome(word, taps, k))
    if not any(s):
        return 0
    if s not in table:
        return None
    word[table[s]] ^= 1
    return 1


def bptc_decode(coded):
    """196 received bits to (12 octets, corrections), or None if uncorrectable."""
    raw = [coded[a * 181 % 196] for a in range(196)]
    matrix = np.array(raw[1:]).reshape(13, 15)
    fixes = 0
    for passes in ('rows', 'columns', 'rows'):
        lines = range(9) if passes == 'rows' else range(15)
        for i in lines:
            word = (matrix[i] if passes == 'rows' else matrix[:, i]).tolist()
            fixed = (correct(word, H15_11, 11, ROW_TABLE) if passes == 'rows'
                     else correct(word, H13_9, 9, COLUMN_TABLE))
            if fixed is None:
                return None
            fixes += fixed
            if passes == 'rows':
                matrix[i] = word
            else:
                matrix[:, i] = word
    data = [matrix[r, c] for r in range(9) for c in range(11) if r or c >= 3]
    return bits_bytes(data), fixes


def slot_type_decode(word):
    """Nearest Golay(20,8) codeword within three errors."""
    best = min(SLOT_WORDS, key=lambda w: bin(w ^ word).count('1'))
    errors = bin(best ^ word).count('1')
    return (*SLOT_WORDS[best], errors) if errors <= 3 else None


def find_bursts(filtered, rate, threshold=.8):
    """Sync positions on a 1/STEP-symbol grid, strongest first, 100 symbols apart."""
    grid = np.arange(0, len(filtered) * SYMBOL_RATE / rate, 1 / STEP)
    y = np.interp(grid, np.arange(len(filtered)) * SYMBOL_RATE / rate, filtered)
    if len(y) < 24 * STEP:
        return y, []
    windows = np.lib.stride_tricks.sliding_window_view(y, 23 * STEP + 1)[:, ::STEP]
    centred = windows - windows.mean(axis=1, keepdims=True)
    energy = np.sqrt(np.sum(centred ** 2, axis=1)) * np.sqrt(24) + 1e-9
    best = np.zeros(len(windows))
    for pattern in SYNC.values():
        bits = int_bits(pattern, 48)
        template = np.array([1 if bits[i + 1] == 1 and bits[i] == 0 else -1
                             for i in range(0, 48, 2)])
        best = np.maximum(best, centred @ template / energy)
    found = []
    for n in np.argsort(best)[::-1]:
        if best[n] < threshold:
            break
        start = n - SYNC_START * STEP
        if start >= 0 and start + 131 * STEP < len(y) and all(abs(n - m) > 100 * STEP for m in found):
            found.append(n)
    return y, sorted(int(n) - SYNC_START * STEP for n in found)


def slice_burst(y, start):
    s = y[start:start + 132 * STEP:STEP]
    # Every DMR sync uses outer symbols only: fit gain and offset to them.
    sync = s[SYNC_START:SYNC_START + 24]
    x = np.sign(sync - sync.mean())
    gain = np.sum(x * (sync - sync.mean())) / np.sum(x * (x - x.mean()))
    offset = sync.mean() - gain * x.mean()
    z = (s - offset) / gain
    dibits = np.where(z >= 2 / 3, 1, np.where(z >= 0, 0, np.where(z >= -2 / 3, 2, 3)))
    bits = [int(b) for d in dibits for b in (d >> 1, d & 1)]
    levels = {f'{d:02b}': round(float(np.mean(s[dibits == d] - offset)), 1)
              for d in range(4) if np.any(dibits == d)}
    return bits, dict(gain_hz=round(float(gain), 1), offset_hz=round(float(offset), 1),
                      levels_hz=levels, symbol_rms_error=round(float(np.sqrt(np.mean(
                          (z - np.array(DEVIATION_HZ)[dibits] / DEVIATION_HZ[1]) ** 2))), 4))


def parse_block(data_type, block):
    """Check the burst's RS or CRC and return its fields, or ok=False."""
    out = dict(info=block.hex())
    if data_type in LC_MASK:
        out['ok'] = block[9:] == bytes(b ^ LC_MASK[data_type] for b in rs129_parity(block[:9]))
        out.update(protect=block[0] >> 7, flco=block[0] & 63, fid=block[1],
                   service_options=block[2], destination=int.from_bytes(block[3:6], 'big'),
                   source=int.from_bytes(block[6:9], 'big'))
        out['call'] = {0: 'group', 3: 'private'}.get(out['flco'], 'other')
    elif data_type in CRC_MASK:
        out['ok'] = int.from_bytes(block[10:], 'big') == crc_ccitt(block[:10]) ^ CRC_MASK[data_type]
        if data_type == CSBK:
            out.update(last_block=block[0] >> 7, protect=block[0] >> 6 & 1, csbko=block[0] & 63,
                       fid=block[1], data_follows=block[2] >> 7, group=block[2] >> 6 & 1,
                       blocks_to_follow=block[3], destination=int.from_bytes(block[4:7], 'big'),
                       source=int.from_bytes(block[7:10], 'big'))
        else:
            out.update(group=block[0] >> 7, response=block[0] >> 6 & 1,
                       header_compression=block[0] >> 5 & 1, dpf=block[0] & 15,
                       sap=block[1] >> 4, pad=(block[0] >> 4 & 1) << 4 | block[1] & 15,
                       destination=int.from_bytes(block[2:5], 'big'),
                       source=int.from_bytes(block[5:8], 'big'), full=block[8] >> 7,
                       blocks=block[8] & 127, fsn=block[9] & 15)
    elif data_type == RATE_HALF_DATA:
        out['ok'] = True                   # checked by CRC-32 across the message
    else:
        out['ok'] = False
    return out


def decode(data, rate):
    freq = discriminator(iq_from_bytes(data), rate)
    y, starts = find_bursts(matched_filter(freq, rate), rate)
    bursts, messages, header, blocks = [], [], None, []
    for start in starts:
        bits, levels = slice_burst(y, start)
        word = int(''.join(map(str, bits[108:156])), 2)
        sync, errors = min(((name, bin(word ^ value).count('1')) for name, value in SYNC.items()),
                           key=lambda item: item[1])
        item = dict(time_s=round(start / STEP / SYMBOL_RATE, 5), sync=sync, sync_errors=errors,
                    bits=bits_bytes(bits).hex(), **levels)
        slot = slot_type_decode(int(''.join(map(str, bits[98:108] + bits[156:166])), 2))
        decoded = bptc_decode(bits[:98] + bits[166:]) if slot else None
        if errors > 4 or not slot or not decoded:
            item['ok'] = False
            bursts.append(item)
            continue
        cc, data_type, slot_errors = slot
        block, fixes = decoded
        item.update(colour_code=cc, data_type=data_type, type=DATA_TYPES.get(data_type, 'other'),
                    slot_type_errors=slot_errors, bptc_corrections=fixes,
                    **parse_block(data_type, block))
        bursts.append(item)
        if data_type == DATA_HEADER and item['ok']:
            header, blocks = item, []
        elif data_type == RATE_HALF_DATA and header:
            blocks.append(block)
            if len(blocks) == header['blocks']:
                message = b''.join(blocks)
                crc_ok = message[-4:] == crc32_dmr(message[:-4]).to_bytes(4, 'little')
                payload = message[:len(message) - 4 - header['pad']]
                messages.append(dict(ok=crc_ok, source=header['source'],
                    destination=header['destination'], sap=header['sap'], payload=payload.hex()))
                for done in bursts[-len(blocks):]:
                    done['ok'] = crc_ok
                header = None
    return dict(bursts=bursts, messages=messages)


def write_wav(path, freq, rate):
    """48 kHz mono discriminator audio for external DSD-style decoders."""
    positions = np.arange(int(len(freq) * 48000 / rate)) * rate / 48000
    pcm = np.clip(np.interp(positions, np.arange(len(freq)), freq) * 5, -32767, 32767)
    with wave.open(str(path), 'wb') as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(48000)
        output.writeframes(pcm.astype('<i2').tobytes())


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('iq8', type=Path)
    p.add_argument('--rate', type=int, help='default: sample_rate from the .json sidecar')
    p.add_argument('--wav', type=Path, help='also write 48 kHz discriminator audio')
    a = p.parse_args()
    rate = a.rate or json.loads(a.iq8.with_suffix('.json').read_text())['sample_rate']
    data = a.iq8.read_bytes()
    result = decode(data, rate)
    if a.wav:
        write_wav(a.wav, discriminator(iq_from_bytes(data), rate), rate)
    print(json.dumps(result, indent=2))
    ok = result['bursts'] and all(b['ok'] for b in result['bursts'])
    raise SystemExit(0 if ok else 1)


if __name__ == '__main__':
    main()
