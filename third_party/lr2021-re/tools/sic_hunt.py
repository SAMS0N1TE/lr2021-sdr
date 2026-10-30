"""Search an unmodified continuous IQ8 recording for offline SIC collision candidates.

Can follow a growing capture in a separate process. A strict candidate needs a new
CRC-valid reception, >10 ms overlap of payload/header symbol intervals with an
accepted cancelled packet, and failure of a timing-hinted decode on original IQ.
This is evidence of a candidate, not a guarantee against every possible false CRC.
"""
import argparse
from fractions import Fraction
import hashlib
import json
from pathlib import Path
import time

import numpy as np
from scipy.signal import resample_poly
from decode_continuous import preamble_regions
from lora_decode import decode, DecodeError
from lora_sic import decode_sic, json_report, plot_waterfall


def same_reception(a, b):
    return a['payload'] == b['payload'] and abs(a['packet_end_s'] - b['packet_end_s']) < .01


def read_summary(path):
    # A recorder creates the file before its final JSON write has completed.
    try:
        return json.loads(path.read_text())
    except (FileNotFoundError, json.JSONDecodeError):
        return None


def inspect_recoveries(iq, fs, result):
    """Conservative overlap and acquisition-only controls on newly decoded frames."""
    ratio = Fraction(62500 / fs).limit_denominator(64)
    original4 = None
    findings = []
    for index, packet in enumerate(result['packets']):
        if any(same_reception(packet, p) for p in result['baseline_packets']):
            continue
        correction = result['cancellations'][index]['timing_correction_chips'] / 62500
        first_data = packet['phy']['data_start_chips'] / 62500 + correction
        end = packet['packet_end_s'] + correction
        overlaps = []
        for previous, stats in zip(result['packets'][:index], result['cancellations'][:index]):
            if not stats['accepted']:
                continue
            delta = stats['timing_correction_chips'] / 62500
            overlap = min(end, previous['packet_end_s'] + delta) - max(
                first_data, previous['phy']['data_start_chips'] / 62500 + delta)
            if overlap > .010:
                overlaps.append(dict(cancelled_payload=previous['payload'].hex(),
                                     payload_symbol_overlap_s=float(overlap)))
        if original4 is None:
            original4 = resample_poly(iq, 4 * ratio.numerator, ratio.denominator)
        control = None
        try:
            trial = decode(original4, 250000, preamble_start_s=packet['packet_start_s'])
            control = dict(crc_ok=bool(trial.get('crc_ok')), payload=trial.get('payload', b'').hex(),
                           packet_end_s=trial.get('packet_end_s'), error=trial.get('error'))
            without_subtraction = bool(trial.get('crc_ok') and same_reception(trial, packet))
        except DecodeError as e:
            without_subtraction = False
            control = dict(crc_ok=False, error=str(e))
        findings.append(dict(payload=packet['payload'].hex(), packet_start_s=packet['packet_start_s'],
                             packet_end_s=packet['packet_end_s'], sic_iteration=index,
                             meshcore=packet.get('meshcore'), overlaps=overlaps,
                             timing_hinted_original_iq_control=control,
                             recovered_without_subtraction=without_subtraction,
                             strict_collision_candidate=bool(overlaps and not without_subtraction)))
    return findings


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('capture', type=Path, help='continuous capture directory containing iq.bin')
    ap.add_argument('--fs', type=float, default=250000 / 3)
    ap.add_argument('--follow', action='store_true')
    ap.add_argument('--out', type=Path)
    ap.add_argument('--stride', type=float, default=1)
    args = ap.parse_args()
    if not np.isfinite((args.fs, args.stride)).all() or args.fs < 62500 or not 0 < args.stride <= 1:
        ap.error('need fs >= 62500 and stride in (0,1] seconds')
    output = args.out or args.capture / 'sic_hunt'
    output.mkdir(parents=True, exist_ok=True)
    source = args.capture / 'iq.bin'
    capture_summary = args.capture / 'summary.json'
    if not args.follow and not source.exists():
        ap.error('capture directory has no iq.bin')
    window, stride = int(4 * args.fs), round(args.stride * args.fs)
    offset = windows = baseline_windows = candidate_count = strict_count = 0
    processed = 0
    baseline_seen, recovered_seen, candidate_seen = [], [], []
    findings_all = []
    started = time.monotonic()
    last_update = 0
    status = dict(complete=False, source=str(source), fs_hz=args.fs, window_s=window / args.fs,
                  stride_samples=stride, method='CRC-gated SIC on unmodified over-the-air IQ windows')

    def save_status(complete=False):
        duration = source.stat().st_size / (2 * args.fs) if source.exists() else 0
        metadata = read_summary(capture_summary)
        status.update(complete=complete, windows=windows, baseline_windows=baseline_windows,
                      baseline_receptions=len(baseline_seen), sic_receptions=len(recovered_seen),
                      new_candidate_receptions=candidate_count, strict_collision_candidates=strict_count,
                      processed_until_s=min(duration, processed / args.fs),
                      retained_iq_s=duration, elapsed_s=time.monotonic() - started,
                      capture_valid=metadata.get('valid') if metadata is not None else None,
                      findings=findings_all)
        (output / 'summary.json').write_text(json.dumps(status, indent=2))

    with (output / 'windows.jsonl').open('w') as log:
        while True:
            done = read_summary(capture_summary) is not None or not args.follow
            available = source.stat().st_size // 2 if source.exists() else 0
            remaining = available - offset
            if remaining < window and not done:
                save_status()
                time.sleep(.2)
                continue
            if remaining < round(.06 * args.fs):
                break
            count = min(window, remaining)
            raw = np.fromfile(source, dtype=np.int8, offset=offset * 2, count=count * 2)
            iq = raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32)
            regions = preamble_regions(iq, args.fs)
            entry = dict(sample_start=offset, sample_count=count, preamble_regions=len(regions))
            if regions:
                result = decode_sic(iq, args.fs, max_packets=6, return_stages=True)
                baseline_windows += bool(result['baseline_packets'])
                for packets, seen in ((result['baseline_packets'], baseline_seen), (result['packets'], recovered_seen)):
                    for packet in packets:
                        stamp = dict(payload=packet['payload'].hex(), packet_end_s=packet['packet_end_s'] + offset / args.fs)
                        if not any(same_reception(stamp, p) for p in seen):
                            seen.append(stamp)
                findings = inspect_recoveries(iq, args.fs, result)
                entry.update(baseline_count=len(result['baseline_packets']), sic_count=len(result['packets']), findings=findings)
                unseen = []
                for finding in findings:
                    stamp = dict(payload=finding['payload'], packet_end_s=finding['packet_end_s'] + offset / args.fs)
                    if not any(same_reception(stamp, p) for p in candidate_seen):
                        unseen.append(finding)
                        candidate_seen.append(stamp)
                if unseen:
                    candidate_count += len(unseen)
                    strict_count += sum(f['strict_collision_candidate'] for f in unseen)
                    directory = output / f'candidate_{candidate_count:04d}'
                    directory.mkdir()
                    raw.tofile(directory / 'iq8.bin')
                    result['residual'].astype('<c8').tofile(directory / 'residual.cf32')
                    report = dict(json_report(result), kind='unmodified over-the-air capture window',
                                  source=str(source), source_sample_start=offset, source_sample_count=count,
                                  iq8_sha256=hashlib.sha256(raw.tobytes()).hexdigest(), findings=findings,
                                  baseline_phy=[p['phy'] for p in result['baseline_packets']],
                                  sic_phy=[p['phy'] for p in result['packets']])
                    (directory / 'report.json').write_text(json.dumps(report, indent=2))
                    plot_waterfall(iq, result, directory / 'waterfall.png',
                                   title='Unmodified over-the-air IQ: SIC candidate; see original-IQ controls')
                    for finding in unseen:
                        findings_all.append(dict(finding, capture_packet_end_s=finding['packet_end_s'] + offset / args.fs,
                                                 artifact=str(directory)))
                    print(f"CANDIDATE {directory}: {sum(f['strict_collision_candidate'] for f in unseen)} strict", flush=True)
            windows += 1
            processed = offset + count
            log.write(json.dumps(entry) + '\n')
            log.flush()
            now = time.monotonic()
            if now - last_update >= 10:
                save_status()
                print(f"Scanned {(offset + count) / args.fs:.1f}s; baseline receptions={len(baseline_seen)} "
                      f"SIC receptions={len(recovered_seen)} strict candidates={strict_count}", flush=True)
                last_update = now
            if done and count < window:
                offset += count
                break
            offset += stride
        status['baseline_reception_list'] = baseline_seen
        status['sic_reception_list'] = recovered_seen
        save_status(complete=True)
    print(json.dumps({k: v for k, v in status.items() if k not in ('findings', 'baseline_reception_list', 'sic_reception_list')}), flush=True)


if __name__ == '__main__':
    main()
