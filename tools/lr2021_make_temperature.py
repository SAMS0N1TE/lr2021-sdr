#!/usr/bin/env python3
"""Generate an Acurite-606TX-format OOK temperature test burst as signed I8/Q8.

Packet layout/timing: rtl_433 25.12, src/devices/acurite.c (protocol 55).
This is a synthetic lab packet, not a reading from a physical sensor.
The current LR2021 TX interface sends it at 915 MHz, not the sensor's RF band.
"""
import argparse
import json
from pathlib import Path


def checksum(data):
    result, key = 0, 0xF1
    for byte in data:
        for bit in range(7, -1, -1):
            if byte & (1 << bit):
                result ^= key
            key = (key >> 1) ^ (0x98 if key & 1 else 0)
    return result


def generate(temperature=23.4, sensor_id=90, channel=1, battery_ok=True,
             sample_rate=9600, rows=6, amplitude=48, corrupt=False):
    if not -40 <= temperature <= 70:
        raise ValueError('Temperature must be -40..70 C')
    if not 0 <= sensor_id <= 255 or channel not in (1, 2, 3):
        raise ValueError('Expected ID 0..255 and channel 1..3')
    if sample_rate not in (4800, 9600, 19200) or not 3 <= rows <= 6:
        raise ValueError('Expected rate 4800/9600/19200 and 3..6 repeated rows')
    if not 1 <= amplitude <= 56:
        raise ValueError('Amplitude must be 1..56')
    raw = round(temperature * 10) & 0xFFF
    packet = bytes([sensor_id, (int(battery_ok) << 7) | ((channel - 1) << 4)
                    | (raw >> 8), raw & 255])
    packet += bytes([checksum(packet) ^ int(corrupt)])
    bits = [(byte >> bit) & 1 for byte in packet for bit in range(7, -1, -1)] + [0]
    samples = bytearray()
    elapsed_us = 0

    def segment(duration_us, level):
        nonlocal elapsed_us
        elapsed_us += duration_us
        # Round cumulative boundaries rather than accumulating timing errors.
        end = round(elapsed_us * sample_rate / 1_000_000)
        samples.extend(bytes([level, 0]) * (end - len(samples) // 2))

    segment(15000, 0)
    for row in range(rows):
        for bit in bits:
            segment(500, amplitude)
            segment(4000 if bit else 2000, 0)
        segment(500, amplitude)
        segment(15000 if row == rows - 1 else 8000, 0)
    if len(samples) > 16384 or len(samples) / 2 / sample_rate > .9:
        raise ValueError('Waveform exceeds upload/duration bounds; reduce rows or rate')
    return bytes(samples), dict(format='Acurite-606TX', synthetic=True,
        temperature_C=round(temperature * 10) / 10, id=sensor_id, channel=channel,
        battery_ok=int(battery_ok), packet_hex=packet.hex(), checksum_valid=not corrupt,
        sample_rate=sample_rate, rows=rows, amplitude=amplitude,
        complex_samples=len(samples)//2, duration_s=len(samples)/2/sample_rate)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('output', type=Path)
    p.add_argument('--temperature', type=float, default=23.4)
    p.add_argument('--id', type=int, default=90)
    p.add_argument('--channel', type=int, default=1)
    p.add_argument('--low-battery', action='store_true')
    p.add_argument('--rate', type=int, default=9600)
    p.add_argument('--rows', type=int, default=6)
    p.add_argument('--amplitude', type=int, default=48)
    p.add_argument('--corrupt-checksum', action='store_true')
    a = p.parse_args()
    data, metadata = generate(a.temperature, a.id, a.channel, not a.low_battery,
        a.rate, a.rows, a.amplitude, a.corrupt_checksum)
    a.output.write_bytes(data)
    a.output.with_suffix('.json').write_text(json.dumps(metadata, indent=2)+'\n')
    print(json.dumps(metadata))


if __name__ == '__main__':
    main()
