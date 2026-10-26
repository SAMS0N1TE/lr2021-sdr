#!/usr/bin/env python3
"""Unpack the source-6 format observed on LR2021 ROM 1.24.

Credit: sfowlr supplied the original LR2021 route/ROM research. Local CW and
fixed-gain controls identify signed six-bit I/Q and a four-bit gain tag.
Gains 1, 7, 8 and 13 were checked; automatic mode yielded tag 13. Dynamic
transition timing and other ROMs remain unverified. This opens no hardware.
"""
import argparse
import json
from pathlib import Path
import numpy as np


def unpack_source6(data):
    """Return signed I6, Q6 and unsigned four-bit tag arrays from FIFO bytes."""
    if len(data) % 2:
        raise ValueError('Source-6 payload must have an even byte count')
    lanes = np.frombuffer(data, dtype=np.int8).astype(np.int16).reshape(-1, 2)
    i = (lanes[:, 0] >> 2).astype(np.int8)
    q = (lanes[:, 1] >> 2).astype(np.int8)
    gain = ((lanes[:, 0] & 3) | ((lanes[:, 1] & 3) << 2)).astype(np.uint8)
    return i, q, gain


def self_test():
    words = np.arange(65536, dtype='<u2')
    original = words.tobytes()
    i, q, gain = unpack_source6(original)
    lo = ((i.astype(np.int16) << 2) & 255) | (gain & 3)
    hi = ((q.astype(np.int16) << 2) & 255) | ((gain >> 2) & 3)
    rebuilt = (lo.astype(np.uint16) | (hi.astype(np.uint16) << 8)).astype('<u2')
    assert rebuilt.tobytes() == original
    assert i.min() == q.min() == -32 and i.max() == q.max() == 31
    assert gain.min() == 0 and gain.max() == 15
    print('All 65536 packing round trips passed; device semantics are measured separately.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path, nargs='?', help='Raw FIFO bytes, two per pair')
    parser.add_argument('--out', type=Path, help='New NPZ containing I, Q and gain_tag')
    parser.add_argument('--self-test', action='store_true')
    args = parser.parse_args()
    if args.self_test:
        self_test()
    if args.capture is None:
        if not args.self_test:
            parser.error('provide a raw capture or --self-test')
        return
    raw = args.capture.read_bytes()
    if not raw:
        parser.error('capture is empty')
    i, q, gain = unpack_source6(raw)
    tags, counts = np.unique(gain, return_counts=True)
    summary = {'pairs': len(i), 'I_range': [int(i.min()), int(i.max())],
               'Q_range': [int(q.min()), int(q.max())],
               'gain_tag_histogram': dict(zip(map(int, tags), map(int, counts))),
               'format': 'observed IQ6 + IQ6 + gain4', 'hardware_demodulation': False}
    if args.out is not None:
        if args.out.exists():
            parser.error('output exists; choose a new path')
        args.out.parent.mkdir(parents=True, exist_ok=True)
        with args.out.open('xb') as f:
            np.savez_compressed(f, I=i, Q=q, gain_tag=gain, metadata_json=json.dumps(summary))
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
