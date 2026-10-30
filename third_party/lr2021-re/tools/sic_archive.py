"""Scan existing real IQ8 captures for SIC recoveries without mixing recordings.

Continuous recordings use the rolling-window hunt; snapshots are decoded whole.
Known SPI-corrupted, incompatible, derived and previously hunted files are excluded.
Results retain provenance and distinguish failed transport prefixes from valid runs.
"""
import argparse
from concurrent.futures import ProcessPoolExecutor, as_completed
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import numpy as np
from lora_decode import load_iq8
from lora_sic import decode_sic, json_report, plot_waterfall
from sic_hunt import inspect_recoveries

ROOT = Path(__file__).resolve().parents[1]


def inventory(root, output):
    tasks, skipped, hashes = [], [], {}
    fs_default = 250000 / 3
    for path in sorted(root.rglob('*.bin')):
        relative = path.relative_to(root)
        parts = relative.parts
        reason = None
        if 'wild_sic' in parts:
            reason = 'already hunted in live experiment, or derived from it'
        elif 'sic_hunt' in parts:
            reason = 'derived analysis window'
        elif parts[0] == 'spi32_delay':
            reason = 'known-corrupted SPI32 experiment'
        kind = 'stream' if path.name == 'iq.bin' else 'snapshot'
        metadata = None
        fs = fs_default
        validity = 'snapshot: no continuous transport footer; historical IQ8/decimation-3 profile assumed'
        if relative.as_posix() in ('stream_rak1.bin', 'stream_rak2.bin'):
            fs = 250000 if path.name == 'stream_rak1.bin' else 125000
            validity = 'snapshot: no continuous transport footer; sample rate documented in companion capture plot'
        if reason is None and kind == 'stream':
            meta_path = path.parent / 'summary.json'
            if not meta_path.exists():
                reason = 'continuous recording lacks format/rate metadata'
            else:
                metadata = json.loads(meta_path.read_text())
                radio = metadata.get('radio') or {}
                fs = metadata.get('sample_rate_hz') or 0
                if metadata.get('spi_mhz') == 32:
                    reason = 'unqualified/corrupted SPI32 experiment'
                elif metadata.get('sample_format', 'iq8') != 'iq8':
                    reason = 'not IQ8; current archive hunt uses IQ8'
                elif radio.get('sf') != 7 or radio.get('bandwidth_hz') != 62500:
                    reason = 'not the supported SF7/BW62.5 profile'
                elif fs < 62500:
                    reason = 'unsupported or missing sample rate'
                elif path.stat().st_size != metadata.get('samples', -1) * 2:
                    reason = 'file length differs from recorded sample count'
                validity = ('complete transport-valid recording' if metadata.get('valid')
                            else 'diagnostic retained prefix; final transport checks failed')
        if reason is None and (path.stat().st_size % 2 or path.stat().st_size / (2 * fs) < .06):
            reason = 'incomplete IQ pair or too short for a complete supported packet'
        if reason is not None:
            skipped.append(dict(source=str(path), reason=reason))
            continue
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        if digest in hashes:
            skipped.append(dict(source=str(path), reason='duplicate file content', duplicate_of=hashes[digest]))
            continue
        hashes[digest] = str(path)
        key = relative.as_posix().replace('/', '__').replace('.bin', '')
        tasks.append(dict(source=str(path), kind=kind, fs_hz=fs, sha256=digest,
                          source_validity=validity, metadata=metadata, output=str(output / key)))
    for path in sorted(root.rglob('*.npy')):
        skipped.append(dict(source=str(path), reason='early short raw capture; no reliable sample-rate/profile metadata'))
    return tasks, skipped


def scan(task):
    source, output = Path(task['source']), Path(task['output'])
    output.mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    base = {k: v for k, v in task.items() if k != 'metadata'}
    try:
        if task['kind'] == 'stream':
            command = [sys.executable, str(ROOT / 'tools/sic_hunt.py'), str(source.parent),
                       '--fs', str(task['fs_hz']), '--out', str(output)]
            with (output / 'hunt.log').open('w') as log:
                process = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
            if process.returncode:
                raise RuntimeError(f'rolling hunt exited {process.returncode}; see {output}/hunt.log')
            status = json.loads((output / 'summary.json').read_text())
            return dict(base, baseline_receptions=status['baseline_receptions'],
                        sic_receptions=status['sic_receptions'], new_receptions=status['new_candidate_receptions'],
                        strict_candidates=status['strict_collision_candidates'],
                        seconds=source.stat().st_size / (2 * task['fs_hz']), windows=status['windows'],
                        elapsed_s=time.monotonic() - started, complete=status['complete'])
        iq = load_iq8(source)
        result = decode_sic(iq, task['fs_hz'], max_packets=6, return_stages=True)
        findings = inspect_recoveries(iq, task['fs_hz'], result)
        report = dict(json_report(result), source=str(source), source_sha256=task['sha256'],
                      source_validity=task['source_validity'], findings=findings,
                      baseline_phy=[p['phy'] for p in result['baseline_packets']],
                      sic_phy=[p['phy'] for p in result['packets']])
        (output / 'report.json').write_text(json.dumps(report, indent=2))
        if findings:
            result['residual'].astype('<c8').tofile(output / 'residual.cf32')
            plot_waterfall(iq, result, output / 'waterfall.png',
                           title='Existing unmodified RF snapshot: inspect provenance and original-IQ controls')
        return dict(base, baseline_receptions=len(result['baseline_packets']),
                    sic_receptions=len(result['packets']), new_receptions=len(findings),
                    strict_candidates=sum(f['strict_collision_candidate'] for f in findings),
                    seconds=len(iq) / task['fs_hz'], windows=1,
                    elapsed_s=time.monotonic() - started, complete=True)
    except Exception as error:
        return dict(base, complete=False, error=repr(error), elapsed_s=time.monotonic() - started)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--captures', type=Path, default=ROOT / 'captures')
    ap.add_argument('--out', type=Path, default=ROOT / 'analysis/sic_archive')
    ap.add_argument('--workers', type=int, choices=range(1, 5), default=3)
    args = ap.parse_args()
    os.environ['OPENBLAS_NUM_THREADS'] = '1'
    args.out.mkdir(parents=True, exist_ok=True)
    tasks, skipped = inventory(args.captures, args.out)
    report = dict(complete=False, kind='existing unmodified RF captures; no synthetic mixing',
                  note='Totals count receptions per source file; separate files may contain repeated/overlapping observations.',
                  planned_files=len(tasks), planned_seconds=sum(Path(t['source']).stat().st_size / (2*t['fs_hz']) for t in tasks),
                  skipped=skipped, files=[])
    (args.out / 'inventory.json').write_text(json.dumps(dict(tasks=tasks, skipped=skipped), indent=2))
    print(f"Scanning {len(tasks)} files / {report['planned_seconds']:.1f} nominal seconds; {len(skipped)} excluded", flush=True)
    with ProcessPoolExecutor(max_workers=args.workers) as pool:
        futures = [pool.submit(scan, task) for task in tasks]
        for future in as_completed(futures):
            row = future.result()
            report['files'].append(row)
            (args.out / 'archive.json').write_text(json.dumps(report, indent=2))
            if row.get('new_receptions') or row.get('error') or len(report['files']) % 10 == 0:
                print(f"{len(report['files'])}/{len(tasks)} {row['source']}: baseline={row.get('baseline_receptions')} "
                      f"SIC={row.get('sic_receptions')} strict={row.get('strict_candidates')} error={row.get('error')}", flush=True)
    rows = report['files']
    report['complete'] = all(r['complete'] for r in rows)
    report['summary'] = dict(files=len(rows), errors=sum(not r['complete'] for r in rows),
                            seconds=sum(r.get('seconds', 0) for r in rows),
                            baseline_receptions=sum(r.get('baseline_receptions', 0) for r in rows),
                            sic_receptions=sum(r.get('sic_receptions', 0) for r in rows),
                            new_receptions=sum(r.get('new_receptions', 0) for r in rows),
                            strict_collision_candidates=sum(r.get('strict_candidates', 0) for r in rows))
    (args.out / 'archive.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report['summary']), flush=True)
    print(f'Archive report: {args.out}/archive.json', flush=True)
    if not report['complete']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
