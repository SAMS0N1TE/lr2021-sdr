"""Reproducible SIC demonstration using a DIGITAL MIXTURE of separate RF captures.

This does not transmit and is not evidence of a simultaneous two-transmitter RF
collision. Ground truth is used to construct/validate the demo, never by SIC.
"""
import argparse
import hashlib
import json
from pathlib import Path

import numpy as np
from lora_decode import decode, load_iq8
from lora_sic import decode_sic, json_report, plot_waterfall

ROOT = Path(__file__).resolve().parents[1]


def mix_recordings(a, b, fs, delay_s=.043, weaker_db=6, cfo_hz=2300, phase_rad=.7,
                   *, noise_snr_db=None, seed=20261028):
    """Normalize whole-capture RMS, delay/frequency/phase shift B, then add.

    Floating-point addition avoids introducing a third, synthetic ADC clipping
    impairment. Existing quantization/noise/distortion in each recording remains.
    Optional complex AWGN is referenced to B's normalized whole-capture power,
    not to noise-free RF signal power. The seed makes the mixture reproducible.
    """
    if (not np.isfinite((fs, delay_s, weaker_db, cfo_hz, phase_rad)).all()
            or fs <= 0 or delay_s < 0 or weaker_db < 0):
        raise ValueError('invalid mixture parameters')
    if noise_snr_db is not None and not np.isfinite(noise_snr_db):
        raise ValueError('noise SNR must be finite')
    signals = []
    for x in (a, b):
        x = np.asarray(x, np.complex64)
        rms = np.sqrt(np.mean(abs(x) ** 2))
        if not np.isfinite(x).all() or not np.isfinite(rms) or rms <= 0:
            raise ValueError('source IQ must be finite and nonzero')
        signals.append(x / rms)
    a, b = signals
    offset = round(delay_s * fs)
    mixture = np.zeros(max(len(a), len(b) + offset), np.complex64)
    mixture[:len(a)] += a
    carrier = np.exp(2j * np.pi * cfo_hz * np.arange(len(b)) / fs + 1j * phase_rad)
    mixture[offset:offset + len(b)] += 10 ** (-weaker_db / 20) * b * carrier
    if noise_snr_db is not None:
        rng = np.random.default_rng(seed)
        sigma = np.sqrt(10 ** (-(weaker_db + noise_snr_db) / 10) / 2)
        noise = sigma * (rng.standard_normal(len(mixture)) + 1j * rng.standard_normal(len(mixture)))
        mixture += noise.astype(np.complex64)
    return mixture


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--a', type=Path, default=ROOT / 'captures/iter/c00.bin')
    ap.add_argument('--b', type=Path, default=ROOT / 'captures/iter/c01.bin')
    ap.add_argument('--fs', type=float, default=250000 / 3)
    ap.add_argument('--delay', type=float, default=.043)
    ap.add_argument('--weaker-db', type=float, default=6)
    ap.add_argument('--cfo', type=float, default=2300, help='additional frequency shift applied to recording B')
    ap.add_argument('--phase', type=float, default=.7)
    ap.add_argument('--noise-snr', type=float,
                    help='added complex AWGN: dB of weaker whole-capture RMS power / noise power')
    ap.add_argument('--seed', type=int, default=20261028)
    ap.add_argument('--out', type=Path, default=ROOT / 'analysis/sic_demo')
    args = ap.parse_args()
    a, b = load_iq8(args.a), load_iq8(args.b)
    original = [decode(x, args.fs) for x in (a, b)]
    if not all(p.get('crc_ok') for p in original) or original[0]['payload'] == original[1]['payload']:
        ap.error('demo needs two different, independently CRC-valid source packets')
    try:
        mixture = mix_recordings(a, b, args.fs, args.delay, args.weaker_db, args.cfo, args.phase,
                                 noise_snr_db=args.noise_snr, seed=args.seed)
    except ValueError as e:
        ap.error(str(e))
    result = decode_sic(mixture, args.fs, return_stages=True)
    expected = {p['payload'] for p in original}
    baseline = {p['payload'] for p in result['baseline_packets']}
    recovered = {p['payload'] for p in result['packets']}
    proved = baseline == {original[0]['payload']} and recovered == expected
    delay = round(args.delay * args.fs) / args.fs
    overlap = max(0, min(original[0]['packet_end_s'], original[1]['packet_end_s'] + delay)
                  - max(original[0]['packet_start_s'], original[1]['packet_start_s'] + delay))
    args.out.mkdir(parents=True, exist_ok=True)
    mixture.astype('<c8').tofile(args.out / 'mixture.cf32')
    result['residual'].astype('<c8').tofile(args.out / 'residual.cf32')
    provenance = dict(kind='digital mixture of separately recorded RF captures; NOT a simultaneous RF collision',
                      sources=[dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                                    payload=p['payload'].hex(), crc_ok=p['crc_ok'])
                               for path, p in zip((args.a, args.b), original)],
                      fs_hz=args.fs, delay_s=delay, approximate_packet_overlap_s=overlap,
                      weak_to_strong_whole_capture_rms_db=-args.weaker_db,
                      additional_weak_cfo_hz=args.cfo, additional_weak_phase_rad=args.phase,
                      added_noise_snr_db=args.noise_snr,
                      added_noise_seed=args.seed if args.noise_snr is not None else None,
                      added_noise_definition='weaker normalized whole-capture power / added complex AWGN power; source noise remains',
                      mixture_sha256=hashlib.sha256(mixture.astype('<c8').tobytes()).hexdigest(),
                      format='little-endian complex float32',
                      validation=dict(baseline_only_strong=baseline == {original[0]['payload']},
                                      recovered_exactly_both_original_payloads=recovered == expected,
                                      demonstrated_additional_recovery=proved))
    (args.out / 'provenance.json').write_text(json.dumps(provenance, indent=2))
    (args.out / 'report.json').write_text(json.dumps(json_report(result), indent=2))
    plot_waterfall(mixture, result, args.out / 'waterfall.png',
                   title='DIGITAL MIXTURE of separately recorded packets; not a simultaneous RF capture')
    print(f"Baseline: {len(result['baseline_packets'])} CRC-valid; SIC: {len(result['packets'])} CRC-valid")
    for p in result['packets']:
        print(f"pass {p['sic_iteration']}: payload SHA256 {hashlib.sha256(p['payload']).hexdigest()[:12]} / {len(p['payload'])} bytes / CRC OK")
    print(f'Exact recovery of both source payloads: {proved}\nArtifacts: {args.out}')
    if not proved:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
