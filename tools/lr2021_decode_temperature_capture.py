#!/usr/bin/env python3
"""Frequency-shift a HackRF recording and run unmodified rtl_433 protocol 55.

Requires NumPy and an external rtl_433 executable. No packet data or expected
temperature is supplied to the decoder. Input: signed interleaved I8/Q8,
2 Msamples/s, receiver tuned 250 kHz below the transmitted signal.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

import numpy as np


def decode(capture, executable):
    data = gzip.decompress(capture.read_bytes()) if capture.suffix == '.gz' else capture.read_bytes()
    if not data or len(data) % 16:
        raise ValueError('Capture must contain a multiple of eight I/Q pairs')
    iq = np.frombuffer(data, np.int8).astype(np.float32).reshape(-1, 2) / 128
    signal = (iq[:, 0] + 1j * iq[:, 1]).reshape(-1, 8)
    # Fixed -250 kHz translation and eight-sample boxcar decimation to 250 ksps.
    # No envelope normalization, fitted timing, pulse reconstruction or bit repair.
    shifted = np.mean(signal * np.exp(-2j * np.pi * np.arange(8) / 8), axis=1)
    with tempfile.TemporaryDirectory(prefix='lr2021-temperature-') as directory:
        converted = Path(directory) / 'capture_250k.cf32'
        shifted.astype('<c8').tofile(converted)
        args = [str(executable.resolve()), '-r', str(converted), '-R', '55', '-F', 'json', '-F', 'log']
        result = subprocess.run(args, capture_output=True, text=True, timeout=30, check=True)
    records = [json.loads(line) for line in result.stdout.splitlines() if line.startswith('{')]
    return dict(capture=capture.name, raw_sha256=hashlib.sha256(data).hexdigest(),
        input_complex_samples=len(data)//2, input_sample_rate=2000000,
        frequency_shift_hz=-250000, output_sample_rate=250000,
        processing='fixed frequency translation and 8-sample mean; no amplitude normalization',
        decoder_sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
        decoder_arguments=['-r', 'capture_250k.cf32', '-R', '55', '-F', 'json', '-F', 'log'],
        decoder_log=result.stderr, records=records)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=Path)
    p.add_argument('--rtl433', required=True, type=Path)
    p.add_argument('--output', type=Path)
    a = p.parse_args()
    text = json.dumps(decode(a.capture, a.rtl433), indent=2) + '\n'
    if a.output:
        a.output.write_text(text, encoding='utf-8')
    print(text)


if __name__ == '__main__':
    main()
