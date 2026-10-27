#!/usr/bin/env python3
"""Generate DMR Tier II mobile-station bursts as bounded signed I8/Q8 playback.

Builds Voice LC Header and Terminator with LC, Preamble CSBK, and unconfirmed
Data Header plus rate 1/2 data bursts (ETSI TS 102 361-1/-2): MS-sourced sync,
Golay(20,8) Slot Type, BPTC(196,96), RS(12,9) or CRC with per-type masks.
Modulation is 4800 symbols/s 4FSK, +/-648 and +/-1944 Hz, root-raised-cosine
alpha 0.2, integrated into phase. One burst per 60 ms TDMA frame, carrier off
in the other slot, as a mobile using one slot in direct mode.
No voice bursts and no vocoder. Requires NumPy. The current LakeShark TX
interface is fixed at 915 MHz/-9 dBm.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np

SYMBOL_RATE = 4800
DEVIATION_HZ = (648, 1944, -648, -1944)    # dibits 00, 01, 10, 11
ALPHA = .2
SPAN = 8                                   # RRC truncated at +/-8 symbols
FRAME_SYMBOLS = 288                        # 60 ms: two 30 ms slots
GUARD_SYMBOLS = 6                          # 1.25 ms ramp either side
BURST_SYMBOLS = 132                        # 27.5 ms, 264 bits
RATES = (19200, 125000)
UPLOAD_SAMPLES = 8192                      # 16,384-byte upload
ROM_SAMPLES = 32767
MAX_SECONDS = .9

SYNC = {'bs_voice': 0x755FD7DF75F7, 'bs_data': 0xDFF57D75DF5D,
        'ms_voice': 0x7F7D5DD57DFD, 'ms_data': 0xD5D7F77FD757}
VOICE_LC_HEADER, TERMINATOR_LC, CSBK, DATA_HEADER, RATE_HALF_DATA = 1, 2, 3, 6, 7
DATA_TYPES = {VOICE_LC_HEADER: 'voice_lc_header', TERMINATOR_LC: 'terminator_lc',
              CSBK: 'csbk', DATA_HEADER: 'data_header', RATE_HALF_DATA: 'rate_half_data'}
LC_MASK = {VOICE_LC_HEADER: 0x96, TERMINATOR_LC: 0x99}
CRC_MASK = {CSBK: 0xA5A5, DATA_HEADER: 0xCCCC}
GROUP_VOICE, UNIT_TO_UNIT_VOICE = 0x00, 0x03       # FLCO
PREAMBLE_CSBKO = 0x3D
UNCONFIRMED_DPF = 0x2
LAST_FRAGMENT_FSN = 0x8

# Hamming parity taps for the BPTC rows (15,11) and columns (13,9),
# checked against MMDVM-Host output via LakeShark's reference vectors.
H15_11 = ((0, 1, 2, 3, 5, 7, 8), (1, 2, 3, 4, 6, 8, 9),
          (2, 3, 4, 5, 7, 9, 10), (0, 1, 2, 4, 6, 7, 10))
H13_9 = ((0, 1, 3, 5, 6), (0, 1, 2, 4, 6, 7),
         (0, 1, 2, 3, 5, 7, 8), (0, 2, 4, 5, 8))


def int_bits(value, count):
    return [value >> (count - 1 - i) & 1 for i in range(count)]


def byte_bits(data):
    return [byte >> (7 - i) & 1 for byte in data for i in range(8)]


def bits_bytes(bits):
    return bytes(int(''.join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))


def parity(data, taps):
    return [sum(data[i] for i in tap) & 1 for tap in taps]


def slot_type(colour_code, data_type):
    """Golay(20,8) Slot Type word: colour code high nibble, data type low."""
    data = (colour_code & 15) << 4 | data_type & 15
    word = data << 11
    for bit in range(18, 10, -1):
        if word >> bit & 1:
            word ^= 0xC75 << (bit - 11)
    word |= data << 11
    return word << 1 | bin(word).count('1') & 1


def gf_multiply(a, b):
    result = 0
    while b:
        if b & 1:
            result ^= a
        a = (a << 1 ^ (0x11D if a & 0x80 else 0)) & 0xFF
        b >>= 1
    return result


def rs129_parity(message):
    """Three RS(12,9) parity bytes over GF(256), in transmitted order."""
    p = [0, 0, 0]
    for byte in message:
        feedback = byte ^ p[2]
        p[2] = p[1] ^ gf_multiply(14, feedback)
        p[1] = p[0] ^ gf_multiply(56, feedback)
        p[0] = gf_multiply(64, feedback)
    return bytes([p[2], p[1], p[0]])


def crc_ccitt(data):
    """16-bit CRC: x^16+x^12+x^5+1, zero preset, inverted result."""
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = (crc << 1 ^ (0x1021 if crc & 0x8000 else 0)) & 0xFFFF
    return crc ^ 0xFFFF


def crc32_msb(data):
    crc = 0
    for byte in data:
        crc ^= byte << 24
        for _ in range(8):
            crc = (crc << 1 ^ (0x04C11DB7 if crc & 0x80000000 else 0)) & 0xFFFFFFFF
    return crc


def crc32_dmr(data):
    """Packet data CRC-32: octets taken in swapped pairs (1, 0, 3, 2, ...)."""
    if len(data) % 2:
        raise ValueError('CRC-32 input must be an even number of octets')
    return crc32_msb(bytes(data[i ^ 1] for i in range(len(data))))


def full_lc(source, destination, private=False, service_options=0, fid=0):
    """72-bit full LC: PF=0, FLCO, FID, service options, destination, source."""
    flco = UNIT_TO_UNIT_VOICE if private else GROUP_VOICE
    return (bytes([flco, fid, service_options]) + destination.to_bytes(3, 'big')
            + source.to_bytes(3, 'big'))


def lc_block(lc, data_type):
    return lc + bytes(byte ^ LC_MASK[data_type] for byte in rs129_parity(lc))


def crc_block(body, data_type):
    return body + (crc_ccitt(body) ^ CRC_MASK[data_type]).to_bytes(2, 'big')


def preamble_csbk(source, destination, blocks_to_follow, group=True, data_follows=True):
    """Preamble CSBK: last block, CSBKO 0x3D, standard FID."""
    body = (bytes([0x80 | PREAMBLE_CSBKO, 0, data_follows << 7 | group << 6, blocks_to_follow])
            + destination.to_bytes(3, 'big') + source.to_bytes(3, 'big'))
    return crc_block(body, CSBK)


def data_header(source, destination, blocks, pad, group=True, sap=4):
    """Unconfirmed data header, single fragment, no response requested."""
    body = (bytes([group << 7 | (pad >> 4 & 1) << 4 | UNCONFIRMED_DPF, sap << 4 | pad & 15])
            + destination.to_bytes(3, 'big') + source.to_bytes(3, 'big')
            + bytes([0x80 | blocks, LAST_FRAGMENT_FSN]))
    return crc_block(body, DATA_HEADER)


def rate_half_blocks(payload):
    """Split user octets into 12-octet blocks; the last ends with CRC-32."""
    blocks = (len(payload) + 4 + 11) // 12
    padded = payload + bytes(12 * blocks - 4 - len(payload))
    message = padded + crc32_dmr(padded).to_bytes(4, 'little')
    return [message[i:i + 12] for i in range(0, len(message), 12)], len(padded) - len(payload)


def bptc_encode(block):
    """BPTC(196,96): 12 octets to 196 interleaved bits."""
    data = byte_bits(block)
    matrix = np.zeros((13, 15), int)
    cells = [(r, c) for r in range(9) for c in range(11) if r or c >= 3]
    for (r, c), bit in zip(cells, data):
        matrix[r, c] = bit
    for r in range(9):
        matrix[r, 11:] = parity(matrix[r, :11], H15_11)
    for c in range(15):
        matrix[9:, c] = parity(matrix[:9, c], H13_9)
    raw = [0] + matrix.ravel().tolist()
    coded = [0] * 196
    for a, bit in enumerate(raw):
        coded[a * 181 % 196] = bit
    return coded


def burst(block, data_type, colour_code, sync='ms_data'):
    """264 burst bits: info, Slot Type, 48-bit sync, Slot Type, info."""
    info = bptc_encode(block)
    st = int_bits(slot_type(colour_code, data_type), 20)
    return info[:98] + st[:10] + int_bits(SYNC[sync], 48) + st[10:] + info[98:]


def entry(block, data_type, colour_code):
    bits = burst(block, data_type, colour_code)
    return dict(type=DATA_TYPES[data_type], data_type=data_type, sync='ms_data',
                info=block.hex(), burst=bits_bytes(bits).hex(), bits=bits)


def check_ids(source, destination, colour_code):
    for name, value in (('Source', source), ('Destination', destination)):
        if not 1 <= value <= 0xFFFFFF:
            raise ValueError(f'{name} ID must be 1..16777215')
    if not 0 <= colour_code <= 15:
        raise ValueError('Colour code must be 0..15')


def voice_call(source, destination, colour_code=1, private=False, headers=2, terminators=2):
    """Voice LC Headers then Terminators with LC; no voice bursts between."""
    check_ids(source, destination, colour_code)
    if headers < 0 or terminators < 0 or headers + terminators < 1:
        raise ValueError('Expected at least one header or terminator')
    lc = full_lc(source, destination, private)
    return ([entry(lc_block(lc, VOICE_LC_HEADER), VOICE_LC_HEADER, colour_code)] * headers
            + [entry(lc_block(lc, TERMINATOR_LC), TERMINATOR_LC, colour_code)] * terminators)


def csbk_bursts(source, destination, colour_code=1, private=False, count=2):
    """Preamble CSBKs counting down blocks to follow."""
    check_ids(source, destination, colour_code)
    if count < 1:
        raise ValueError('Expected at least one CSBK')
    return [entry(preamble_csbk(source, destination, count - 1 - i, not private, False),
                  CSBK, colour_code) for i in range(count)]


def data_call(payload, source, destination, colour_code=1, private=False, preambles=1, sap=4):
    """Preamble CSBKs, an unconfirmed data header and rate 1/2 blocks."""
    check_ids(source, destination, colour_code)
    if not payload:
        raise ValueError('Empty data payload')
    if preambles < 0 or not 0 <= sap <= 15:
        raise ValueError('Expected preambles >= 0 and SAP 0..15')
    blocks, pad = rate_half_blocks(payload)
    if len(blocks) > 127:
        raise ValueError('Payload exceeds 127 data blocks')
    after = 1 + len(blocks)
    return ([entry(preamble_csbk(source, destination, preambles - 1 - i + after, not private),
                   CSBK, colour_code) for i in range(preambles)]
            + [entry(data_header(source, destination, len(blocks), pad, not private, sap),
                     DATA_HEADER, colour_code)]
            + [entry(block, RATE_HALF_DATA, colour_code) for block in blocks])


def rrc(t, alpha=ALPHA):
    """Root-raised-cosine impulse response, t in symbols, zero beyond SPAN."""
    t = np.asarray(t, float)
    out = np.zeros_like(t)
    zero = np.isclose(t, 0)
    edge = np.isclose(np.abs(t), 1 / (4 * alpha))
    rest = ~(zero | edge)
    x = t[rest]
    out[rest] = ((np.sin(np.pi * x * (1 - alpha)) + 4 * alpha * x * np.cos(np.pi * x * (1 + alpha)))
                 / (np.pi * x * (1 - (4 * alpha * x) ** 2)))
    out[zero] = 1 + alpha * (4 / np.pi - 1)
    out[edge] = alpha / np.sqrt(2) * ((1 + 2 / np.pi) * np.sin(np.pi / (4 * alpha))
                                      + (1 - 2 / np.pi) * np.cos(np.pi / (4 * alpha)))
    out[np.abs(t) > SPAN] = 0
    return out


def modulate(bursts, rate=19200, amplitude=48):
    if not bursts:
        raise ValueError('No bursts to modulate')
    if rate not in RATES:
        raise ValueError('Expected 19200 or 125000 samples/s; 76,800 halves FM deviation '
                         'and the other rates are untested for FSK (docs/dmr-tx.md)')
    if not 1 <= amplitude <= 55:
        raise ValueError('Amplitude must be 1..55 (rounding must stay within radius 56)')
    frame = rate * FRAME_SYMBOLS // SYMBOL_RATE
    count = len(bursts) * frame
    if count > UPLOAD_SAMPLES:
        raise ValueError(f'{len(bursts)} bursts need {count} samples at {rate} samples/s; '
                         f'the 16 KiB upload holds {UPLOAD_SAMPLES} samples, '
                         f'{UPLOAD_SAMPLES // frame} burst(s) at this rate')
    t = np.arange(count) * SYMBOL_RATE / rate          # time in symbols
    deviation = np.zeros(count)
    envelope = np.zeros(count)
    scale = rrc(np.arange(-SPAN, SPAN + 1)).sum()
    for n, item in enumerate(bursts):
        bits = item['bits']
        levels = np.array(DEVIATION_HZ)[[bits[i] << 1 | bits[i + 1] for i in range(0, 264, 2)]]
        start = n * FRAME_SYMBOLS + GUARD_SYMBOLS
        centres = start + .5 + np.arange(BURST_SYMBOLS)
        window = (t > start - SPAN) & (t < start + BURST_SYMBOLS + SPAN)
        deviation[window] += rrc(t[window, None] - centres[None, :]) @ levels / scale
        # Raised-cosine power ramp inside each 1.25 ms guard.
        x = t - start
        rise = np.clip((x + GUARD_SYMBOLS) / GUARD_SYMBOLS, 0, 1)
        fall = np.clip((BURST_SYMBOLS + GUARD_SYMBOLS - x) / GUARD_SYMBOLS, 0, 1)
        envelope += .5 - .5 * np.cos(np.pi * np.minimum(rise, fall))
    phase = 2 * np.pi * np.cumsum(deviation) / rate
    iq = amplitude * envelope * np.exp(1j * phase)
    pairs = np.rint(np.column_stack([iq.real, iq.imag])).astype(np.int8)
    data = pairs.tobytes()
    repeats = min(16, ROM_SAMPLES // count, int(MAX_SECONDS * rate // count))
    return data, dict(sample_rate=rate, symbol_rate=SYMBOL_RATE, sample_count=count,
        duration_s=count / rate, amplitude=amplitude, modulation='DMR 4FSK',
        nominal_dibit_deviation_hz=list(DEVIATION_HZ), rrc_alpha=ALPHA,
        rrc_span_symbols=SPAN, frame_period_s=FRAME_SYMBOLS / SYMBOL_RATE,
        guard_symbols=GUARD_SYMBOLS, peak_deviation_hz=round(float(np.max(
            np.abs(deviation[envelope > .5]))), 1), max_repeats=repeats,
        iq8_sha256=hashlib.sha256(data).hexdigest(),
        bursts=[{k: v for k, v in item.items() if k != 'bits'} for item in bursts])


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('kind', choices=['voice', 'csbk', 'data'],
                   help='voice: LC headers and terminators; csbk: preamble CSBKs; '
                        'data: preamble CSBKs, data header, rate 1/2 blocks')
    p.add_argument('output', type=Path)
    p.add_argument('--source', type=int, default=123456)
    p.add_argument('--destination', type=int, default=4242)
    p.add_argument('--private', action='store_true', help='individual instead of group call')
    p.add_argument('--colour-code', '--cc', type=int, default=1)
    p.add_argument('--headers', type=int, default=2, help='voice: Voice LC Headers')
    p.add_argument('--terminators', type=int, default=2, help='voice: Terminators with LC')
    p.add_argument('--count', type=int, default=2, help='csbk: preamble CSBKs')
    p.add_argument('--preambles', type=int, default=1, help='data: preamble CSBKs')
    p.add_argument('--text', default='LR2021 DMR', help='data: ASCII payload')
    p.add_argument('--hex', help='data: payload as hex, instead of --text')
    p.add_argument('--sap', type=int, default=4, help='data: service access point')
    p.add_argument('--rate', type=int, default=19200)
    p.add_argument('--amplitude', type=int, default=48)
    a = p.parse_args()
    ids = (a.source, a.destination, a.colour_code, a.private)
    try:
        if a.kind == 'voice':
            bursts = voice_call(*ids, a.headers, a.terminators)
        elif a.kind == 'csbk':
            bursts = csbk_bursts(*ids, a.count)
        else:
            payload = bytes.fromhex(a.hex) if a.hex else a.text.encode('ascii')
            bursts = data_call(payload, *ids, a.preambles, a.sap)
        data, metadata = modulate(bursts, a.rate, a.amplitude)
    except ValueError as error:
        p.error(str(error))
    metadata.update(kind=a.kind, source=a.source, destination=a.destination,
                    call='private' if a.private else 'group', colour_code=a.colour_code)
    if a.kind == 'data':
        metadata.update(payload=payload.hex(), sap=a.sap)
    a.output.write_bytes(data)
    a.output.with_suffix('.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps({k: v for k, v in metadata.items() if k != 'bursts'}))


if __name__ == '__main__':
    main()
