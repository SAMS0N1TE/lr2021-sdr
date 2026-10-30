"""Sweep deterministic synthetic LoRa collisions made from existing IQ8 captures.

One noise realization per cell is an acceptance experiment, not a PER estimate.
Known source payloads are used only for scoring, never as inputs to the canceller.
"""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import time

import numpy as np
from lora_decode import decode, load_iq8
from lora_sic import decode_sic
from sic_demo import mix_recordings, ROOT


def plot_results(report, path):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.colors import BoundaryNorm, ListedColormap

    delays, gaps, noises = report['delay_s'], report['weaker_db'], report['noise_snr_db']
    fig, axes = plt.subplots(len(delays), 2, figsize=(11, 4 * len(delays)), squeeze=False, layout='constrained')
    cmap = ListedColormap(['#243247', '#f2bd56', '#45ba89'])
    norm = BoundaryNorm([-.5, .5, 1.5, 2.5], 3)
    for row, delay in enumerate(delays):
        for col, key in enumerate(('baseline_correct', 'sic_correct')):
            grid = np.zeros((len(noises), len(gaps)), int)
            for case in report['cases']:
                if case['delay_s'] == delay:
                    grid[noises.index(case['noise_snr_db']), gaps.index(case['weaker_db'])] = case[key]
            ax = axes[row, col]
            image = ax.imshow(grid, cmap=cmap, norm=norm, aspect='auto')
            ax.set_xticks(range(len(gaps)), [str(gap) for gap in gaps])
            ax.set_yticks(range(len(noises)), ['none' if noise is None else str(noise) for noise in noises])
            ax.set(title=f"{'Ordinary scan' if col == 0 else 'After SIC'}; {delay * 1000:g} ms start delay",
                   xlabel='Weaker-recording attenuation (dB)',
                   ylabel='Weaker power / added noise (dB)')
            for j in range(len(noises)):
                for i in range(len(gaps)):
                    ax.text(i, j, str(grid[j, i]), ha='center', va='center',
                            color='white' if grid[j, i] == 0 else '#162127', fontsize=15)
    fig.colorbar(image, ax=list(axes.flat), ticks=[0, 1, 2], label='Correct original payloads recovered')
    fig.suptitle('Synthetic overlaps from two separately recorded RF packets\n'
                 'One deterministic noise realization per cell; not a packet-error-rate estimate')
    fig.savefig(path, dpi=150)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--a', type=Path, default=ROOT / 'captures/iter/c00.bin')
    ap.add_argument('--b', type=Path, default=ROOT / 'captures/iter/c01.bin')
    ap.add_argument('--fs', type=float, default=250000 / 3)
    ap.add_argument('--delays', type=float, nargs='+', default=[.043, .119])
    ap.add_argument('--gaps', type=float, nargs='+', default=[3, 6, 9, 12])
    ap.add_argument('--noise-snrs', nargs='+', default=['none', '10', '0', '-10', '-15'])
    ap.add_argument('--cfo', type=float, default=2300)
    ap.add_argument('--phase', type=float, default=.7)
    ap.add_argument('--seed', type=int, default=20261028)
    ap.add_argument('--out', type=Path, default=ROOT / 'analysis/sic_sweep')
    args = ap.parse_args()
    try:
        noises = [None if v.lower() == 'none' else float(v) for v in args.noise_snrs]
    except ValueError:
        ap.error('--noise-snrs needs numbers or none')
    if (not np.isfinite([args.fs, args.cfo, args.phase, *args.delays, *args.gaps]).all()
            or args.fs <= 0 or args.seed < 0 or min(args.delays + args.gaps) < 0
            or not all(v is None or np.isfinite(v) for v in noises)):
        ap.error('invalid sweep parameters')
    if len(set(noises)) != len(noises) or len(set(args.delays)) != len(args.delays) or len(set(args.gaps)) != len(args.gaps):
        ap.error('sweep axes must not contain duplicates')
    a, b = load_iq8(args.a), load_iq8(args.b)
    originals = [decode(signal, args.fs) for signal in (a, b)]
    expected = {p['payload'] for p in originals if p.get('crc_ok')}
    if len(expected) != 2:
        ap.error('need two different independently CRC-valid source packets')
    if max(len(a), len(b) + round(max(args.delays) * args.fs)) / args.fs > 4:
        ap.error('all mixtures must fit in the four-second SIC window')
    args.out.mkdir(parents=True, exist_ok=True)
    report = dict(kind='digital mixtures of two separate RF recordings; NOT simultaneous RF collisions',
                  complete=False, fs_hz=args.fs, seed=args.seed,
                  delay_s=args.delays, weaker_db=args.gaps, noise_snr_db=noises,
                  noise_definition='weaker normalized whole-capture power / added complex AWGN power; source noise remains',
                  additional_weak_cfo_hz=args.cfo, additional_weak_phase_rad=args.phase,
                  sources=[dict(path=str(path), sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                                payload=p['payload'].hex()) for path, p in zip((args.a, args.b), originals)],
                  cases=[])
    for delay in args.delays:
        for noise in noises:
            for gap in args.gaps:
                mixture = mix_recordings(a, b, args.fs, delay, gap, args.cfo, args.phase,
                                         noise_snr_db=noise, seed=args.seed)
                started = time.monotonic()
                result = decode_sic(mixture, args.fs, max_packets=4)
                baseline = {p['payload'] for p in result['baseline_packets']}
                sic = {p['payload'] for p in result['packets']}
                case = dict(delay_s=delay, weaker_db=gap, noise_snr_db=noise,
                            baseline_correct=len(baseline & expected), sic_correct=len(sic & expected),
                            exact_both=sic == expected and len(result['packets']) == 2,
                            unexpected_crc_payloads=[p.hex() for p in (baseline | sic) - expected],
                            baseline_payloads=sorted(p.hex() for p in baseline),
                            sic_payloads=sorted(p.hex() for p in sic),
                            mixture_sha256=hashlib.sha256(mixture.astype('<c8').tobytes()).hexdigest(),
                            elapsed_s=time.monotonic() - started)
                report['cases'].append(case)
                (args.out / 'sweep.json').write_text(json.dumps(report, indent=2))
                print(f"delay={delay:g}s attenuation={gap:g}dB added-noise-SNR={noise}: "
                      f"baseline={case['baseline_correct']} SIC={case['sic_correct']}", flush=True)
    cases = report['cases']
    report['complete'] = True
    report['summary'] = dict(cases=len(cases), baseline_both=sum(c['baseline_correct'] == 2 for c in cases),
                            sic_both=sum(c['exact_both'] for c in cases),
                            improved=sum(c['sic_correct'] > c['baseline_correct'] for c in cases),
                            regressed=sum(c['sic_correct'] < c['baseline_correct'] for c in cases),
                            unexpected_crc_cases=sum(bool(c['unexpected_crc_payloads']) for c in cases))
    (args.out / 'sweep.json').write_text(json.dumps(report, indent=2))
    with (args.out / 'sweep.csv').open('w', newline='') as output:
        keys = ['delay_s', 'weaker_db', 'noise_snr_db', 'baseline_correct', 'sic_correct', 'exact_both', 'elapsed_s']
        writer = csv.DictWriter(output, fieldnames=keys, extrasaction='ignore')
        writer.writeheader()
        writer.writerows(cases)
    plot_results(report, args.out / 'recovery.png')
    print(json.dumps(report['summary']), flush=True)
    print(f'Artifacts: {args.out}', flush=True)
    if report['summary']['unexpected_crc_cases']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
