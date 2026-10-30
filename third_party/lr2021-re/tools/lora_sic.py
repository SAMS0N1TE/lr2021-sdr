"""Offline, CRC-gated LoRa successive interference cancellation.

Experimental SF7 / explicit header / CRC path. Fits a short complex FIR separately
per known chirp to accommodate receive filtering and slow phase/gain drift. The
fitter only sees the mixture and CRC-valid receiver decisions, never source IQ.
"""
import argparse
from fractions import Fraction
import json
from pathlib import Path

import numpy as np
from scipy.signal import resample_poly
from scipy.optimize import minimize_scalar

from lora_decode import decode, DecodeError, _bits, _int, _gray
from decode_continuous import preamble_regions
from meshcore_decode import summarize_packet


def _hamming_encode(nibble, cr):
    c = _bits(nibble, 4)[::-1]
    parity = ([sum(c) % 2] if cr == 1 else
              [c[0] ^ c[1] ^ c[2], c[1] ^ c[2] ^ c[3],
               c[0] ^ c[1] ^ c[3], c[0] ^ c[2] ^ c[3]][:cr])
    return _int(c + parity)


def _interleave(nibbles, sf_app, cr, reduced=False):
    words = [_bits(_hamming_encode(n, cr), cr + 4) for n in nibbles]
    inverse = { _gray(i): i for i in range(1 << (sf_app + (2 if reduced else 0))) }
    symbols = []
    for i in range(cr + 4):
        bits = [words[(i - j - 1) % sf_app][i] for j in range(sf_app)]
        if reduced:
            bits += [sum(bits) % 2, 0]
        symbols.append((inverse[_int(bits)] + 1) % len(inverse))
    return symbols


def corrected_symbols(packet):
    """Regenerate FEC symbols from decoded nibbles, including corrected errors.

    Header reduced-rate parity bits are restored, unlike the hard FFT decisions.
    Decoded final-block padding is retained; CRC gates the meaningful payload.
    """
    p = packet['phy']
    sf = p['sf']
    symbols = _interleave(p['header_nibbles'] + p['data_nibbles'][:sf - 7],
                          sf - 2, 4, reduced=True)
    data = p['data_nibbles'][sf - 7:]
    for start in range(0, len(data), sf):
        block = data[start:start + sf]
        if len(block) == sf:
            symbols += _interleave(block, sf, packet['cr'])
    return symbols


def waveform(packet, length, *, oversample=4, timing_chips=0):
    """Ideal complex chirps on the receiver's oversampled coordinate grid."""
    p = packet['phy']
    n = 1 << p['sf']
    size = oversample * n
    data0 = round((p['data_start_chips'] + timing_chips) * oversample)
    first = data0 - round((p['up_symbols'] + 2.25) * size)
    segments = []
    for j in range(p['up_symbols']):
        symbol = (0 if j < p['up_symbols'] - 2 else (8, 16)[j - p['up_symbols'] + 2])
        segments.append((first + j * size, size, symbol, False))
    sync0 = first + p['up_symbols'] * size
    segments.extend([(sync0, size, 0, True), (sync0 + size, size, 0, True),
                     (sync0 + 2 * size, size // 4, 0, True)])
    segments += [(data0 + j * size, size, s, False)
                 for j, s in enumerate(corrected_symbols(packet))]
    model = np.zeros(length, np.complex128)
    occupied = []
    for start, count, symbol, down in segments:
        a, b = max(start, 0), min(start + count, length)
        if b <= a:
            continue
        t = np.arange(a - start, b - start) / oversample
        u = (t + symbol) % n
        phase = np.pi * (u * u / n - u)
        if down:
            phase = -phase
        cfo = packet['cfo_hz'] + timing_chips * p['bw_hz'] / n
        phase += 2 * np.pi * np.arange(a, b) * cfo / (p['bw_hz'] * oversample)
        model[a:b] = np.exp(1j * phase)
        occupied.append((a, b))
    return model, occupied


def cancel_packet(iq4, packet, taps=17):
    """Subtract a local FIR projection onto a CRC-verified waveform.

    Returns a new array. No subtraction occurs on non-CRC packets. The short FIR
    models linear receive filtering/fractional timing; per-chirp coefficients
    accommodate phase resets and slow drift. It cannot undo ADC clipping.
    """
    if not packet.get('crc_ok') or 'phy' not in packet:
        raise ValueError('cancellation requires CRC-valid PHY decisions')
    if taps < 1 or taps > 33 or taps % 2 != 1:
        raise ValueError('FIR taps must be odd and between 1 and 33')
    # The legacy decoder's CFO includes an integer timing ambiguity. Upchirps
    # measure CFO+STO, downchirps CFO-STO. Resolve that ambiguity before building
    # cyclic chirps; treating its correction as physical CFO gives bad replicas.
    p = packet['phy']
    n, rate = 1 << p['sf'], 4 * p['bw_hz']
    sync0 = round((p['data_start_chips'] - 2.25 * n) * 4)
    t = np.arange(4 * n) / 4
    sample = iq4[sync0:sync0 + 4 * n]
    if len(sample) != len(t):
        raise ValueError('truncated sync for cancellation')
    tone = sample * np.exp(1j * np.pi * (t * t / n - t))
    bin_hz = p['bw_hz'] / n
    peak = np.fft.fftfreq(len(tone), 1 / rate)[np.argmax(abs(np.fft.fft(tone)))]
    clock = np.arange(len(tone)) / rate
    fit = minimize_scalar(lambda hz: -abs(np.sum(tone * np.exp(-2j * np.pi * hz * clock))) ** 2,
                          bounds=(peak - bin_hz / 2, peak + bin_hz / 2), method='bounded')
    timing = (fit.x - packet['cfo_hz']) / (2 * bin_hz)
    # Up/down chirps leave a half-symbol ambiguity. Score its three nearby
    # representatives on held, uniformly spaced known chirps. A decoder grid
    # may be more than a quarter-symbol away from the physical boundary.
    alternatives = []
    for shift in (timing, timing - n / 2, timing + n / 2):
        model, segments = waveform(packet, len(iq4), timing_chips=shift)
        score = 0.0
        for a, b in segments[::max(1, len(segments) // 24)]:
            z, y = model[a:b], iq4[a:b]
            score += abs(np.vdot(z, y)) ** 2 / max(np.vdot(z, z).real, 1e-20)
        alternatives.append((score, shift))
    timing = max(alternatives)[1]
    model, segments = waveform(packet, len(iq4), timing_chips=timing)
    half = taps // 2
    padded = np.pad(model, (half, half))
    matrix = np.lib.stride_tricks.sliding_window_view(padded, taps)
    predicted = np.zeros(len(iq4), np.complex128)
    before = after = 0.0
    correlations = []
    for a, b in segments:
        # More observations than unknowns; ridge limits the ill-conditioned
        # delayed-chirp basis and its ability to fit unrelated weak signals.
        if b - a < taps * 4:
            continue
        basis = matrix[a:b]
        target = iq4[a:b]
        gram = basis.conj().T @ basis
        ridge = .01 * np.trace(gram).real / taps
        coef = np.linalg.solve(gram + ridge * np.eye(taps), basis.conj().T @ target)
        estimate = basis @ coef
        energy = float(np.vdot(target, target).real)
        remaining = float(np.vdot(target - estimate, target - estimate).real)
        if remaining > energy:
            continue
        predicted[a:b] = estimate
        before += energy
        after += remaining
        correlations.append(1 - remaining / max(energy, 1e-20))
    residual = np.asarray(iq4 - predicted, np.complex64)
    stats = dict(fir_taps=taps, segments=len(correlations), timing_correction_chips=float(timing),
                 physical_cfo_hz=float(packet['cfo_hz'] + timing * bin_hz),
                 occupied_power_reduction_db=float(10 * np.log10(max(before, 1e-20) / max(after, 1e-20))),
                 median_explained_power=float(np.median(correlations)) if correlations else 0)
    return residual, stats


def _candidates(iq, fs, sf, bw):
    offsets = [0]
    # Reuse stable upchirp acquisition; cancellation never uses the source IQ.
    offsets += [a for a, _ in preamble_regions(iq, fs)]
    found = []
    for a in dict.fromkeys(offsets):
        if len(iq) - a < 25 * (1 << sf) * fs / bw:
            continue
        try:
            p = decode(iq[a:], fs, sf, bw, preamble_start_s=0 if a else None, return_phy=True)
        except (DecodeError, ValueError):
            continue
        if p.get('crc_ok'):
            end = p['packet_end_s'] + a / fs
            if not any(p['payload'] == q['payload'] and abs(end - q['packet_end_s']) < .01 for q in found):
                p['packet_start_s'] += a / fs
                p['packet_end_s'] = end
                p['phy']['grid_start_chips'] += a * bw / fs
                p['phy']['data_start_chips'] += a * bw / fs
                found.append(p)
    return found


def decode_sic(iq, fs, *, sf=7, bw=62500.0, max_packets=4, taps=17, return_stages=False):
    """Return CRC-valid packets, cancellation diagnostics, and complex residual IQ.

    This first implementation deliberately limits the acquisition/PHY profile to
    SF7/BW62.5. Baseline scans use the same acquisition, without subtraction.
    """
    if sf != 7 or bw != 62500:
        raise ValueError('SIC currently supports SF7/BW62.5 only')
    if not 1 <= max_packets <= 16:
        raise ValueError('max_packets must be 1..16')
    if not np.isfinite(fs) or fs <= 0:
        raise ValueError('sample rate must be finite and positive')
    if taps < 1 or taps > 33 or taps % 2 != 1:
        raise ValueError('FIR taps must be odd and between 1 and 33')
    iq = np.asarray(iq, np.complex64)
    if iq.ndim != 1 or len(iq) < 25 * (1 << sf) * fs / bw or fs < bw:
        raise ValueError('need a nonempty packet-sized IQ vector with fs >= bandwidth')
    if not np.isfinite(iq).all():
        raise ValueError('IQ contains non-finite samples')
    if len(iq) / fs > 4:
        raise ValueError('SIC currently uses <=4-second windows; select --start/--seconds')
    ratio = Fraction(bw / fs).limit_denominator(64)
    residual = resample_poly(iq, 4 * ratio.numerator, ratio.denominator)
    observed = residual
    rate = 4 * bw
    baseline = _candidates(residual, rate, sf, bw)
    pending = baseline
    packets, steps, stages = [], [], []
    for iteration in range(max_packets):
        choices = [p for p in pending if not any(
            p['payload'] == q['payload'] and abs(p['packet_end_s'] - q['packet_end_s']) < .01
            for q in packets)]
        if not choices:
            break
        # Prefer the strongest decoded packet by mean energy in its interval.
        def power(p):
            a = max(0, round(p['packet_start_s'] * rate))
            b = min(len(residual), round(p['packet_end_s'] * rate))
            return float(np.mean(abs(residual[a:b]) ** 2)) if b > a else 0
        packet = max(choices, key=power)
        packet['sic_iteration'] = iteration
        candidate, stats = cancel_packet(residual, packet, taps)
        accepted = stats['occupied_power_reduction_db'] >= 1 and stats['median_explained_power'] >= .4
        stats['accepted'] = accepted
        if accepted:
            residual = candidate
        if return_stages:
            removed = resample_poly(observed - residual, ratio.denominator, 4 * ratio.numerator)[:len(iq)]
            stages.append(np.asarray(iq - removed, np.complex64))
        packets.append(packet)
        steps.append(dict(iteration=iteration, payload=packet['payload'].hex(), **stats))
        if not accepted:
            break
        pending = _candidates(residual, rate, sf, bw) if iteration + 1 < max_packets else []
    removed = resample_poly(observed - residual, ratio.denominator, 4 * ratio.numerator)[:len(iq)]
    restored = iq - removed
    return dict(packets=packets, baseline_packets=baseline, cancellations=steps, stages=stages,
                residual=np.asarray(restored, np.complex64), fs_hz=fs)


def json_report(result):
    def packet(p):
        return {k: (v.hex() if isinstance(v, bytes) else v) for k, v in p.items() if k != 'phy'}
    return dict(fs_hz=result['fs_hz'], baseline_packets=[packet(p) for p in result['baseline_packets']],
                packets=[packet(p) for p in result['packets']], cancellations=result['cancellations'])


def plot_waterfall(iq, result, path, *, title):
    """Use one color scale for the mixture and each cancellation residual."""
    import hashlib
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.ticker import MaxNLocator, FormatStrFormatter
    from scipy.signal import stft

    fs = result['fs_hz']
    signals = [iq] + result['stages']
    labels = [f"Before cancellation: {len(result['baseline_packets'])} CRC-valid packet(s)"]
    labels += [f"After pass {i + 1}: {'subtraction accepted' if s['accepted'] else 'poor fit; subtraction rejected'}"
               for i, s in enumerate(result['cancellations'])]
    spectra = []
    for signal in signals:
        f, t, z = stft(signal, fs=fs, nperseg=48, noverlap=32, nfft=128,
                       return_onesided=False, boundary=None)
        spectra.append((np.fft.fftshift(f), t, 20 * np.log10(np.maximum(abs(np.fft.fftshift(z, axes=0)), 1e-12))))
    peak = float(np.max(spectra[0][2]))
    zoom0 = max((p['packet_start_s'] for p in result['packets']), default=0) + .015
    zoom0 = max(0, min(zoom0, len(iq) / fs - .020))
    fig, axes = plt.subplots(len(signals), 2, figsize=(14, 3 * len(signals)), sharex='col', sharey=True,
                             width_ratios=(2, 1), squeeze=False, layout='constrained')
    for row, label, (f, t, level) in zip(axes, labels, spectra):
        for column, ax in enumerate(row):
            selected = np.ones(len(t), bool) if column == 0 else (t >= zoom0) & (t <= zoom0 + .020)
            im = ax.pcolormesh(t[selected], f / 1000, level[:, selected] - peak, vmin=-45, vmax=0,
                              shading='auto', cmap='magma', rasterized=True)
            ax.set_title(label if column == 0 else '20 ms chirp detail')
            if column == 0:
                ax.set_ylabel('Baseband frequency (kHz)')
                for packet in result['packets']:
                    ax.axvline(packet['packet_start_s'], color='cyan', alpha=.6, linewidth=.7)
                    ax.axvline(packet['packet_end_s'], color='cyan', alpha=.6, linewidth=.7)
            else:
                ax.set_xlim(zoom0, zoom0 + .020)
                ax.xaxis.set_major_locator(MaxNLocator(5))
                ax.xaxis.set_major_formatter(FormatStrFormatter('%.3f'))
    axes[-1, 0].set_xlabel('Time within selected IQ window (s)')
    axes[-1, 1].set_xlabel('Time within selected IQ window (s)')
    ids = [f"#{i + 1}: CRC OK, {len(p['payload'])} bytes, SHA256 {hashlib.sha256(p['payload']).hexdigest()[:12]}"
           for i, p in enumerate(result['packets'])]
    fig.suptitle(title + '\n' + ' | '.join(ids), fontsize=10)
    fig.colorbar(im, ax=list(axes.flat), label='Relative spectral level (dB; shared scale)')
    fig.savefig(path, dpi=150)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('path', type=Path)
    ap.add_argument('--fs', type=float, default=250000 / 3)
    ap.add_argument('--format', choices=('iq8', 'iq16', 'cf32'), default='iq8')
    ap.add_argument('--max-packets', type=int, default=4)
    ap.add_argument('--taps', type=int, default=17)
    ap.add_argument('--start', type=float, default=0, help='start offset in the input file, seconds')
    ap.add_argument('--seconds', type=float, help='length of selected window, at most 4 seconds')
    ap.add_argument('--plot', action='store_true', help='save waterfall.png (requires matplotlib)')
    ap.add_argument('--out', type=Path, default=Path('analysis/sic'))
    args = ap.parse_args()
    dtype = dict(iq8=np.int8, iq16='<i2', cf32='<c8')[args.format]
    if not np.isfinite(args.fs) or args.fs <= 0 or not np.isfinite(args.start) or args.start < 0:
        ap.error('sample rate must be positive and start must be nonnegative')
    if args.seconds is not None and (not np.isfinite(args.seconds) or not 0 < args.seconds <= 4):
        ap.error('--seconds must be positive and at most 4')
    pair_bytes = 8 if args.format == 'cf32' else 2 * np.dtype(dtype).itemsize
    if args.path.stat().st_size % pair_bytes:
        ap.error('incomplete IQ pair')
    start = round(args.start * args.fs)
    available = max(0, args.path.stat().st_size // pair_bytes - start)
    if min(available / args.fs, args.seconds if args.seconds is not None else np.inf) > 4:
        ap.error('select a <=4-second window with --start/--seconds')
    count = round(args.seconds * args.fs) if args.seconds is not None else -1
    count = count if args.format == 'cf32' else count * 2 if count >= 0 else -1
    raw = np.fromfile(args.path, dtype=dtype, offset=start * pair_bytes, count=count)
    if args.format == 'cf32':
        iq = raw
    else:
        if len(raw) % 2:
            ap.error('incomplete interleaved IQ pair')
        iq = raw[0::2].astype(np.float32) + 1j * raw[1::2].astype(np.float32)
    try:
        result = decode_sic(iq, args.fs, max_packets=args.max_packets, taps=args.taps, return_stages=args.plot)
    except ValueError as e:
        ap.error(str(e))
    args.out.mkdir(parents=True, exist_ok=True)
    report = dict(json_report(result), input_file=str(args.path), input_start_s=args.start,
                  input_format=args.format, method='experimental CRC-gated per-chirp FIR SIC')
    (args.out / 'report.json').write_text(json.dumps(report, indent=2))
    result['residual'].astype('<c8').tofile(args.out / 'residual.cf32')
    if args.plot:
        plot_waterfall(iq, result, args.out / 'waterfall.png', title='Offline IQ cancellation; RF collision provenance unverified')
    for p in result['packets']:
        detail = summarize_packet(p['meshcore']) if 'meshcore' in p else p['payload'].hex()
        print(f"pass {p['sic_iteration']}: CRC OK {detail}")
    print(f"Baseline {len(result['baseline_packets'])}; SIC {len(result['packets'])}; results: {args.out}")


if __name__ == '__main__':
    main()
