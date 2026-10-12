#!/usr/bin/env python3
"""FM-demodulate a 2 Msps HackRF recording, then run external stock DSD-neo.

Requires NumPy and DSD-neo. Input is signed I8/Q8 (.gz accepted), with receiver
LO 250 kHz below the signal. Audio is written to files, never to speakers.
No expected packet bits or identifiers are given to the decoder.
"""
import argparse
from collections import Counter
import gzip
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile
import wave

import numpy as np


def write_wav(path, pcm, rate=48000):
    with wave.open(str(path), 'wb') as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(pcm.astype('<i2').tobytes())


def demodulate(raw):
    if not raw or len(raw) % 64:
        raise ValueError('Expected a multiple of 32 signed I/Q pairs')
    pairs = np.frombuffer(raw, np.int8).reshape(-1, 2).astype(float) / 128
    iq = (pairs[:, 0] + 1j * pairs[:, 1]).reshape(-1, 32)
    signal = np.mean(iq * np.exp(-2j * np.pi * np.arange(32) / 8), axis=1)
    rate = 62500
    n = np.arange(-64, 65)
    taps = 2 * 7000 / rate * np.sinc(2 * 7000 / rate * n) * np.hamming(len(n))
    taps /= taps.sum()
    signal = np.convolve(signal, taps, 'same')
    discriminator = np.angle(signal[1:] * signal[:-1].conj()) * rate / (2 * np.pi)
    discriminator[np.abs(signal[:-1]) < .008] = 0
    positions = np.arange(round(len(discriminator) * 48000 / rate)) * rate / 48000
    pcm = np.interp(positions, np.arange(len(discriminator)), discriminator) * 5
    return np.clip(pcm, -32767, 32767).astype('<i2')


def decode(capture, executable, output):
    raw = gzip.decompress(capture.read_bytes()) if capture.suffix == '.gz' else capture.read_bytes()
    output.mkdir(parents=True, exist_ok=True)
    pcm = demodulate(raw)
    clean = lambda text: re.sub(r'\x1b\[[0-9;]*m', '', text).replace('\r', '')
    with tempfile.TemporaryDirectory(prefix='lr2021-p25-') as temporary:
        work = Path(temporary)
        source = work / 'discriminator.wav'
        trace = work / 'frames.log'
        write_wav(source, pcm)
        args = [str(executable.resolve()), '-f1', '-mc', '-i', str(source),
                '-o', 'null', '--frame-log', str(trace), '--frontend', 'none']
        result = subprocess.run(args, capture_output=True, text=True, timeout=30, check=True)
        log = clean(result.stdout + result.stderr)
        frames = clean(trace.read_text())
        decoded = re.findall(r'FRAME IMBE .*?data=([0-9A-F]{22}) err=\[(\d+)\] \[(\d+)\]', frames)
        # Repackage only the decoder's recovered 88-bit frames, preserving its
        # error counts. Stock DSD-neo file playback then performs vocoding.
        imbe = b'.imb' + b''.join(bytes([int(second)]) + bytes.fromhex(data)
                                for data, first, second in decoded)
        recovered = work / 'recovered.imb'
        recovered.write_bytes(imbe)
        audio = work / 'recovered.wav'
        audio_stats = None
        if decoded:
            voice = subprocess.run([str(executable.resolve()), '-o', 'null',
                '-w', str(audio), '-r', str(recovered)], capture_output=True,
                text=True, timeout=30, check=True)
            (output / 'vocoder.log').write_text(clean(voice.stdout + voice.stderr), encoding='utf-8')
            with wave.open(str(audio)) as w:
                audio_rate, channels, count = w.getframerate(), w.getnchannels(), w.getnframes()
                samples = np.frombuffer(w.readframes(count), '<i2').reshape(-1, channels)[:, 0]
            if len(samples):
                spectrum = abs(np.fft.rfft(samples.astype(float) * np.hanning(len(samples))))
                frequencies = np.fft.rfftfreq(len(samples), 1 / audio_rate)
                audio_stats = dict(sample_rate=audio_rate, channels=channels,
                    frames=count, duration_s=count/audio_rate,
                    peak_frequency_hz=float(frequencies[np.argmax(spectrum)]))
            (output / 'recovered.wav').write_bytes(audio.read_bytes())
        (output / 'recovered.imb').write_bytes(imbe)
        (output / 'discriminator.wav').write_bytes(source.read_bytes())
    (output / 'decoder.log').write_text(log, encoding='utf-8')
    (output / 'frames.log').write_text(frames, encoding='utf-8')
    matches = re.findall(r'NAC/CC: ([0-9A-Fa-f]+);\s+(LDU1|LDU2|TDU|HDU)', log)
    summary = dict(capture=capture.name, raw_sha256=hashlib.sha256(raw).hexdigest(),
        decoder_sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
        input_sample_rate=2000000, rx_offset_hz=250000, discriminator_sample_rate=48000,
        preprocessing='fixed downmix/decimation, 7 kHz FIR, FM discriminator, .008 amplitude gate, fixed PCM gain 5',
        frame_types=dict(Counter(kind for nac, kind in matches)),
        nac_values=sorted(set(nac for nac, kind in matches)),
        group_source_pairs=sorted(set(re.findall(r'Group (\d+) Source (\d+)', log))),
        imbe_frames=len(decoded), imbe_data_hex=[data for data, first, second in decoded],
        imbe_error_counts=[[int(first), int(second)] for data, first, second in decoded],
        decoder_error_totals={key: int(value) for key, value in re.findall(
            r'Total (audio errors|header errors|irrecoverable header errors): (\d+)', log)},
        audio=audio_stats)
    (output / 'summary.json').write_text(json.dumps(summary, indent=2)+'\n', encoding='utf-8')
    return summary


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=Path)
    p.add_argument('--dsd', required=True, type=Path)
    p.add_argument('--output', required=True, type=Path)
    a = p.parse_args()
    print(json.dumps(decode(a.capture, a.dsd, a.output), indent=2))


if __name__ == '__main__':
    main()
