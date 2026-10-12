#!/usr/bin/env python3
"""Modulate packed P25 Phase 1 test frames into bounded signed I8/Q8 playback.

Input contains four MSB-first dibits per byte, including sync/NID/status/FEC.
This tool modulates existing packets; it is not a P25 voice encoder.
Requires NumPy. The current LakeShark TX interface is fixed at 915 MHz/-9 dBm.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np


def modulate(packet, rate=19200, amplitude=48):
    if not packet:
        raise ValueError('Empty packed P25 packet stream')
    if rate not in (19200, 38400, 76800):
        raise ValueError('Expected a supported integer multiple of 4800 symbols/s')
    if not 1 <= amplitude <= 55:
        raise ValueError('Amplitude must be 1..55 (rounding must stay within radius 56)')
    sps = rate // 4800
    # Alternating outer symbols allow acquisition before the first frame sync.
    dibits = np.array([(byte >> shift) & 3 for byte in packet for shift in (6, 4, 2, 0)])
    symbols = np.r_[np.tile([1, 3], 48), dibits, np.tile([1, 3], 24)]
    if len(symbols) * sps > 8192:
        raise ValueError('Waveform exceeds the 16 KiB upload limit')
    levels = np.array([600, 1800, -600, -1800])[symbols]

    # C4FM transmit response H(f) * P(f), as described by OP25's
    # op25_c4fm_mod.py. Construct a finite 13-symbol impulse response.
    frequency = np.arange(rate // 2 + 1)
    response = np.zeros(len(frequency))
    response[frequency < 1920] = 1
    transition = (frequency >= 1920) & (frequency <= 2880)
    response[transition] = .5 + .5 * np.cos(2 * np.pi * frequency[transition] / 1920)
    response /= np.sinc(frequency / 4800)
    impulse = np.fft.fftshift(np.fft.irfft(response, n=rate))
    count = 13 * sps | 1
    taps = impulse[rate // 2 - count // 2:rate // 2 + count // 2 + 1]
    taps *= sps / taps.sum()
    impulses = np.zeros(len(levels) * sps)
    impulses[::sps] = levels
    deviation = np.convolve(impulses, taps, mode='same')
    phase = 2 * np.pi * np.cumsum(deviation) / rate
    iq = amplitude * np.exp(1j * phase)
    pairs = np.rint(np.column_stack([iq.real, iq.imag])).astype(np.int8)
    return pairs.tobytes(), dict(sample_rate=rate, symbol_rate=4800,
        sample_count=len(iq), duration_s=len(iq) / rate, amplitude=amplitude,
        packed_bytes=len(packet), packed_sha256=hashlib.sha256(packet).hexdigest(),
        prefix_symbols=96, suffix_symbols=48, modulation='C4FM',
        nominal_dibit_deviation_hz=[600, 1800, -600, -1800])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('packet', type=Path)
    p.add_argument('output', type=Path)
    p.add_argument('--rate', type=int, default=19200)
    p.add_argument('--amplitude', type=int, default=48)
    a = p.parse_args()
    data, metadata = modulate(a.packet.read_bytes(), a.rate, a.amplitude)
    a.output.write_bytes(data)
    a.output.with_suffix('.json').write_text(json.dumps(metadata, indent=2)+'\n')
    print(json.dumps(metadata))


if __name__ == '__main__':
    main()
