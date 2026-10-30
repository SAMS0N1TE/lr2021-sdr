# SPDX-License-Identifier: MIT
import argparse, csv, math, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from bridge import Bridge, BridgeError, CAP_GAIN_READBACK, CS12, CS16, EBUSY, iq6_steps, to_iq

TABLE = [16, 28, 38, 49, 57, 64, 73, 85, 96, 108, 120, 127, 138]


def db(x):
    return 20 * math.log10(x) if x > 0 else -200.0


def levels(iq, rate, tone_offset_hz):
    if not len(iq):
        return dict(rms_dbfs=-200.0, peak_dbfs=-200.0, tone_dbfs=-200.0, floor_dbfs=-200.0)
    peak = float(np.max(np.maximum(np.abs(iq.real), np.abs(iq.imag))))
    rms = float(np.sqrt(np.mean(np.abs(iq) ** 2)))
    n = 1 << int(math.log2(min(len(iq), 16384)))
    w = np.hanning(n)
    spec = np.abs(np.fft.fftshift(np.fft.fft(iq[-n:] * w))) / w.sum()
    freqs = np.fft.fftshift(np.fft.fftfreq(n, 1.0 / rate))
    if tone_offset_hz is None:
        k = int(np.argmax(spec))
    else:
        k = int(np.argmin(np.abs(freqs - tone_offset_hz)))
        k = max(0, min(n - 1, k))
        lo, hi = max(0, k - 3), min(n, k + 4)
        k = lo + int(np.argmax(spec[lo:hi]))
    floor = float(np.median(spec))
    return dict(rms_dbfs=db(rms), peak_dbfs=db(peak), tone_dbfs=db(float(spec[k])), floor_dbfs=db(floor),
                tone_hz=float(freqs[k]))


def collect(b, rate, seconds, want_gain=True, on_block=None):
    chunks, gains, frames = [], [], 0
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        got = b.read(0.2)
        if got is None:
            if b.ends:
                break
            continue
        h, raw = got
        frames += 1
        chunks.append(to_iq(h, raw))
        if on_block:
            on_block(h, raw)
        if want_gain and frames % 8 == 0:
            try:
                g = b.get_gain()
                g['host_time'] = time.monotonic()
                gains.append(g)
            except BridgeError as e:
                if e.status != EBUSY:
                    raise
    iq = np.concatenate(chunks) if chunks else np.zeros(0, complex)
    return iq, gains


def open_bridge(a):
    b = Bridge.open(a.device, a.baud)
    if not b.hello_info['caps'] & CAP_GAIN_READBACK:
        sys.exit('device does not advertise cap 256 (gain readback); update its firmware')
    return b


def fmt_for(a):
    return CS12 if a.cs12 else CS16


def sweep(a):
    b = open_bridge(a)
    out = csv.writer(a.out)
    out.writerow(['step_set', 'idle_status', 'idle_step', 'idle_manual', 'idle_value', 'idle_rssi', 'expect_value',
                  'stream_samples', 'stream_step', 'stream_manual', 'stream_value', 'stream_rssi',
                  'rms_dbfs', 'peak_dbfs', 'tone_dbfs', 'floor_dbfs', 'tone_hz', 'dropped'])
    steps = [int(s) for s in a.steps.split(',')]
    for step in steps:
        _, rate = b.set_rx(a.freq, a.bw, step, a.boost, a.path)
        g = b.get_gain()
        b.rx_start(0, fmt_for(a))
        iq, gains = collect(b, rate, a.ms / 1000.0)
        end = b.rx_stop()
        lv = levels(iq[len(iq) // 4:], rate, a.tone_offset)
        s = gains[-1] if gains else dict(step='', manual='', gain_value='', rssi_raw='')
        out.writerow([step, g['status'], g['step'], g['manual'], g['gain_value'], g['rssi_raw'],
                      TABLE[step - 1] if 1 <= step <= 13 else '', len(gains), s['step'], s['manual'],
                      s['gain_value'], s['rssi_raw'], f"{lv['rms_dbfs']:.2f}", f"{lv['peak_dbfs']:.2f}",
                      f"{lv['tone_dbfs']:.2f}", f"{lv['floor_dbfs']:.2f}", f"{lv.get('tone_hz', 0):.0f}",
                      end['dropped']])
        a.out.flush()
    b.close()


def log(a):
    b = open_bridge(a)
    _, rate = b.set_rx(a.freq, a.bw, a.step, a.boost, a.path)
    out = csv.writer(a.out)
    out.writerow(['t_s', 'device_time_us', 'chip_status', 'chip_step', 'chip_manual', 'chip_value', 'chip_rssi',
                  'host_step', 'rms_dbfs', 'peak_dbfs'])
    host_step = a.step if a.step else 13
    if a.host_agc and not a.step:
        sys.exit('--host-agc needs a fixed starting step, for example --step 13')
    quiet_since = None
    b.rx_start(0, fmt_for(a))
    t0 = time.monotonic()
    next_log = t0
    window = []
    while time.monotonic() - t0 < a.seconds:
        got = b.read(0.2)
        if got is None:
            if b.ends:
                break
            continue
        h, raw = got
        iq = to_iq(h, raw)
        window.append(iq)
        peak = db(float(np.max(np.maximum(np.abs(iq.real), np.abs(iq.imag))))) if len(iq) else -200.0
        now = time.monotonic()
        if a.host_agc:
            if peak > -2.0 and host_step > 1:
                host_step -= 1
                b.set_gain(host_step, a.boost)
                quiet_since = None
            elif peak < -15.0:
                quiet_since = quiet_since or now
                if now - quiet_since >= 0.3 and host_step < 13:
                    host_step += 1
                    b.set_gain(host_step, a.boost)
                    quiet_since = now
            else:
                quiet_since = None
        if now >= next_log:
            next_log += a.interval_ms / 1000.0
            block = np.concatenate(window) if window else np.zeros(0, complex)
            window = []
            lv = levels(block, rate, None)
            try:
                g = b.get_gain()
            except BridgeError as e:
                if e.status != EBUSY:
                    raise
                g = dict(status=EBUSY, device_time='', step='', manual='', gain_value='', rssi_raw='')
            out.writerow([f'{now - t0:.3f}', g['device_time'], g['status'], g['step'], g['manual'], g['gain_value'],
                          g['rssi_raw'], host_step if a.host_agc else '', f"{lv['rms_dbfs']:.2f}",
                          f"{lv['peak_dbfs']:.2f}"])
            a.out.flush()
    end = b.rx_stop()
    print(f"# pairs {end['pairs']} dropped {end['dropped']} gain_changes {end['gain_changes']} "
          f"capture_error {end['capture_error']}", file=sys.stderr)
    b.close()


def iq6(a):
    b = open_bridge(a)
    _, rate = b.set_rx(a.freq, 0, a.step, a.boost, a.path, 3, a.code, a.rate)
    out = csv.writer(a.out)
    out.writerow(['t_s', 'iq6_pairs', 'iq6_mode', 'iq6_min', 'iq6_max', 'chip_step', 'chip_manual', 'agree'])
    b.rx_start(int(a.seconds * 1000), CS16)
    t0 = time.monotonic()
    next_log = t0
    seen = []
    while True:
        got = b.read(0.5)
        if got is None:
            if b.ends or time.monotonic() - t0 > a.seconds + 120:
                break
            continue
        s = iq6_steps(*got)
        if s is not None:
            seen.append(s)
        now = time.monotonic()
        if now >= next_log and seen and not b.ends:
            next_log = now + a.interval_ms / 1000.0
            v = np.concatenate(seen)
            seen = []
            mode = int(np.bincount(v, minlength=16).argmax())
            try:
                g = b.get_gain(timeout=30)
            except BridgeError as e:
                if e.status != EBUSY:
                    raise
                continue
            out.writerow([f'{now - t0:.3f}', len(v), mode, int(v.min()), int(v.max()), g['step'], g['manual'],
                          int(mode == g['step'])])
            a.out.flush()
    end = b.wait_end(30)
    print(f"# pairs {end['pairs']} in {end['elapsed_us']} us ({end['pairs'] * 1e6 / max(end['elapsed_us'], 1):.0f}/s), "
          f"header rate {rate}, dropped {end['dropped']}, capture_error {end['capture_error']}", file=sys.stderr)
    b.close()


def main(argv=None):
    p = argparse.ArgumentParser(description='Chip gain readback experiments over the bridge protocol')
    p.add_argument('device', help='tcp:HOST[:PORT] or a serial port such as COM5 or /dev/ttyACM0')
    p.add_argument('--baud', type=int, default=3000000)
    p.add_argument('--out', default='-', help='CSV file, - for standard output')
    p.add_argument('--freq', type=int, default=433900000)
    p.add_argument('--bw', type=int, default=31250)
    p.add_argument('--boost', type=int, default=0)
    p.add_argument('--path', type=int, default=2)
    p.add_argument('--cs12', action='store_true', help='CS12 samples, for the 3 to 4 Mbaud serial link')
    sub = p.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('sweep', help='experiments 1 and 3: each step forced, read back idle and while streaming')
    s.add_argument('--steps', default='1,2,3,4,5,6,7,8,9,10,11,12,13')
    s.add_argument('--ms', type=int, default=400)
    s.add_argument('--tone-offset', type=float, default=None, help='carrier offset from --freq in Hz')
    s.set_defaults(func=sweep)
    s = sub.add_parser('log', help='experiment 2: chip step over time, optionally with host gain control')
    s.add_argument('--step', type=int, default=0, help='0 = chip AGC')
    s.add_argument('--seconds', type=float, default=30)
    s.add_argument('--interval-ms', type=int, default=100)
    s.add_argument('--host-agc', action='store_true')
    s.set_defaults(func=log)
    s = sub.add_parser('iq6', help='experiment 4: source 6 gain bits against the register readback')
    s.add_argument('--step', type=int, default=0)
    s.add_argument('--code', type=int, default=17)
    s.add_argument('--rate', type=int, default=250000)
    s.add_argument('--seconds', type=float, default=10)
    s.add_argument('--interval-ms', type=int, default=100)
    s.set_defaults(func=iq6)
    a = p.parse_args(argv)
    a.out = sys.stdout if a.out == '-' else open(a.out, 'w', newline='')
    a.func(a)


if __name__ == '__main__':
    main()
