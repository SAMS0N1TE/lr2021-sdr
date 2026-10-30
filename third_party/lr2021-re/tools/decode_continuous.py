"""Decode an IQ8 or little-endian IQ16 recording using overlapping windows, optionally following a live file.

The file is the queue: slow decoding does not discard incoming samples. Currently
supports the validated SF7/BW62.5 mode; transport continuity is checked by the recorder.
"""
import argparse
import json
from pathlib import Path
import threading
import time
from fractions import Fraction

import numpy as np
from scipy.signal import resample_poly
from compare_rx import airtime
from lora_decode import decode, DecodeError
from meshcore_decode import summarize_packet


def preamble_regions(data, fs, sample_format='iq8'):
    """Find stable upchirp runs without depending on AGC output power edges."""
    if np.iscomplexobj(data):
        iq = np.asarray(data, dtype=np.complex64)
    else:
        b = np.frombuffer(data, dtype='<i2' if sample_format == 'iq16' else np.int8).astype(np.float32)
        iq = b[0::2] + 1j * b[1::2]
    fraction = Fraction(62500 / fs).limit_denominator(64)
    chips = resample_poly(iq, fraction.numerator, fraction.denominator)
    n = 128
    count = len(chips) // n
    if count < 5:
        return []
    blocks = chips[:count * n].reshape(-1, n)
    t = np.arange(n)
    up = np.exp(1j * np.pi * (t * t / n - t))
    u = abs(np.fft.fft(blocks * up.conj(), axis=1))
    d = abs(np.fft.fft(blocks * up, axis=1))
    peaks = u.argmax(axis=1)
    maximum = u.max(axis=1)
    coherent = ((maximum / (d.max(axis=1) + 1e-9) > 1.8)
                & (maximum ** 2 / ((u ** 2).sum(axis=1) + 1e-9) > .18))
    starts, run = [], None
    for i in range(count + 1):
        stable = i < count and coherent[i]
        if stable and run is not None:
            stable = abs((int(peaks[i]) - int(peaks[i - 1]) + n // 2) % n - n // 2) <= 1
        if not stable:
            if run is not None and i - run >= 5:
                starts.append(run)
            run = i if i < count and coherent[i] else None
        elif run is None:
            run = i
    regions = []
    for index, start in enumerate(starts):
        a = max(0, round((start - 2) * n / 62500 * fs))
        end = starts[index + 1] + 1 if index + 1 < len(starts) else count + 1
        z = min(len(iq), round(end * n / 62500 * fs))
        regions.append((a, z))
    return regions


def decode_window(data, fs, sample_format='iq8'):
    packets = []
    pair_bytes = 4 if sample_format == 'iq16' else 2
    for a, b in preamble_regions(data, fs, sample_format):
        raw = np.frombuffer(data[a * pair_bytes:b * pair_bytes], dtype='<i2' if sample_format == 'iq16' else np.int8).astype(np.float32)
        try:
            packet = decode(raw[0::2] + 1j * raw[1::2], fs, preamble_start_s=0)
        except DecodeError:
            continue
        if packet.get('crc_ok'):
            packet['payload'] = packet['payload'].hex()
            packet['packet_end_s'] += a / fs
            packet['packet_start_s'] = (packet['packet_end_s']
                                        - airtime(packet['len'], cr=packet['cr'] + 4))
            packets.append(packet)
    return packets


def decode_file(path, fs, done, on_packet, window_s=2, stride_s=.5, sample_format='iq8'):
    if sample_format not in ('iq8', 'iq16'):
        raise ValueError('invalid IQ sample format')
    pair_bytes = 4 if sample_format == 'iq16' else 2
    if window_s < 1.9 or not 0 < stride_s <= .5:
        raise ValueError('SF7 windows need >=1.9 seconds with stride <=0.5 seconds')
    window, stride = round(window_s * fs), round(stride_s * fs)
    offset, seen = 0, []
    result = dict(packets=0, windows=0, max_lag_s=0.0)
    with Path(path).open('rb') as source:
        while True:
            available = Path(path).stat().st_size // pair_bytes
            remaining = available - offset
            if remaining < window and not done.is_set():
                done.wait(.1)
                continue
            if remaining <= 0:
                break
            count = min(window, remaining)
            if count < int(.05 * fs):
                break
            source.seek(offset * pair_bytes)
            data = source.read(count * pair_bytes)
            result['max_lag_s'] = max(result['max_lag_s'], max(0, (available - offset - count) / fs))
            packets = sorted(decode_window(data, fs, sample_format), key=lambda a: a['packet_end_s'])
            for packet in packets:
                start = offset / fs + packet['packet_start_s']
                end = offset / fs + packet['packet_end_s']
                # Grid ambiguity can move the estimate by a chirp; RF repeats
                # are separated by much more than this tolerance.
                if any(payload == packet['payload'] and abs(end - previous) < .01
                       for payload, previous in seen):
                    continue
                seen.append((packet['payload'], end))
                event = dict(packet, kind='packet', t_start_s=start, t_end_s=end,
                             sample_start=round(start * fs), sample_end=round(end * fs))
                on_packet(event)
                result['packets'] += 1
            seen = [(payload, end) for payload, end in seen if end >= offset / fs - .02]
            result['windows'] += 1
            if count < window and done.is_set():
                break
            offset += stride
    return result


class FileDecoder:
    def __init__(self, path, fs, output=None, on_packet=None, sample_format='iq8'):
        self.path, self.fs, self.output = Path(path), fs, output
        self.on_packet = on_packet
        self.sample_format = sample_format
        self.done = threading.Event()
        self.result = None
        self.thread = threading.Thread(target=self._run, daemon=True)

    def _run(self):
        try:
            with (Path(self.output).open('w') if self.output else open('/dev/null', 'w')) as log:
                def packet(event):
                    log.write(json.dumps(event) + '\n')
                    log.flush()
                    if self.on_packet:
                        self.on_packet(event)
                self.result = decode_file(self.path, self.fs, self.done, packet, sample_format=self.sample_format)
        except Exception as e:
            self.result = dict(error=repr(e))

    def start(self):
        self.thread.start()

    def finish(self):
        self.done.set()
        self.thread.join()
        return self.result


def print_packet(event):
    detail = (summarize_packet(event['meshcore']) if 'meshcore' in event
              else event.get('meshcore_error', event['payload']))
    print(f"{event['t_end_s']:10.3f}s CRC OK {detail}", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('path', type=Path)
    ap.add_argument('--fs', type=float, default=250000 / 3)
    ap.add_argument('--format', choices=('iq8', 'iq16'), default='iq8')
    ap.add_argument('--follow', action='store_true')
    ap.add_argument('--output', type=Path)
    args = ap.parse_args()
    decoder = FileDecoder(args.path, args.fs, args.output, print_packet, sample_format=args.format)
    if not args.follow:
        decoder.done.set()
    decoder.start()
    try:
        while decoder.thread.is_alive():
            decoder.thread.join(.5)
    except KeyboardInterrupt:
        pass
    print(json.dumps(decoder.finish()), flush=True)


if __name__ == '__main__':
    main()
