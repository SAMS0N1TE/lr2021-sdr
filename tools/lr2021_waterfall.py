# SPDX-License-Identifier: MIT
"""Spectrum computed on the LR2021 itself, through the bridge firmware console.

The chip runs lr2021-re's Patch RAM program (third_party/lr2021-re/firmware/pram): it captures raw
I/Q, runs a 256-point FFT and converts to dB on its own CPU. The ESP32-P4 only reads 256 bytes per
frame back. `wfrun` streams those frames; `pram sweep` steps the receiver and reads RSSI.

  python tools/lr2021_waterfall.py COM14 waterfall --seconds 10 --overlap --plot wf.png
  python tools/lr2021_waterfall.py COM14 sweep 902e6 928e6 25e3 --out band.csv --plot band.png
"""
import argparse, struct, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lr2021_console import open_port, run

FRAME = 12 + 256


def waterfall(s, nseg, seconds, overlap, baud, bw):
    cmd = f'wfrun {nseg} 0 {int(overlap)} {baud}' + (f' {bw}' if bw else '')
    s.reset_input_buffer()
    # One port reconfiguration mid-stream (the baud): a second one (timeout) right after it
    # left the CH343 receiving nothing for ~9 s on Windows.
    s.timeout = 0.05
    s.write((cmd + '\r\n').encode())
    buf, end = b'', time.time() + 5
    while time.time() < end and not (b'WFBIN' in buf and b'\n' in buf[buf.find(b'WFBIN'):]):
        buf += s.read(4096)
        if b'wfrun error' in buf or b'usage:' in buf:
            raise SystemExit(buf.decode(errors='replace'))
    if b'WFBIN' not in buf:
        raise SystemExit(f'wfrun did not start: {buf[-200:]!r}')
    i = buf.index(b'WFBIN')
    head = buf[i:buf.index(b'\n', i)].split()
    rate = int(head[2]) if len(head) > 2 else 250000
    freq = int(head[3]) if len(head) > 3 else 0
    if baud:
        s.baudrate = baud   # the board waits 100 ms before its first frame; frame sync drops the rest
        buf = b''
    else:
        buf = buf[buf.index(b'\n', i) + 1:]
    rows, times, chip, lost, last = [], [], [], 0, None
    stop_at, stopping, end, quiet = time.time() + seconds, False, None, time.time()
    while True:
        if not stopping and time.time() >= stop_at:
            s.write(b'x')
            stopping, end = True, time.time() + 5
        d = s.read(4096)   # large reads stall the CH343 driver on Windows for seconds
        if d:
            quiet = time.time()
        buf += d
        while True:
            k = buf.find(b'\xA5\x5A')
            if k < 0 or len(buf) - k < FRAME:
                break
            if k:
                lost += 1
            seq, t_us, c_us = struct.unpack('<HII', buf[k + 2:k + 12])
            if last is not None and seq != (last + 1) & 0xFFFF:
                lost += 1
            last = seq
            rows.append(buf[k + 12:k + FRAME])
            times.append(t_us / 1e6)
            chip.append(c_us / 1e3)
            buf = buf[k + FRAME:]
        # The board stops within a frame of the 'x'; with a fast UART its closing text comes at 115200.
        if stopping and (time.time() > end or time.time() - quiet > 0.3 or b'wfrun done' in buf):
            break
    if baud:
        s.baudrate = 115200
        buf = b''
    s.timeout = 0.5
    tail = buf + s.read(4096)
    done = [l for l in tail.decode(errors='replace').splitlines() if 'wfrun' in l]
    return rows, times, chip, rate, freq, lost, done[-1] if done else ''


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('port')
    sub = ap.add_subparsers(dest='what', required=True)
    sub.add_parser('load', help='upload the PRAM program and check it (the other commands do this when needed)')
    p = sub.add_parser('waterfall')
    p.add_argument('--seconds', type=float, default=10)
    p.add_argument('--nseg', type=int, default=1, help='256-sample segments averaged per frame (1..10)')
    p.add_argument('--overlap', action='store_true', help='capture the next frame while the chip computes this one (nseg 1)')
    p.add_argument('--baud', type=int, default=2000000, help='UART rate for the frames; 0 keeps 115200 (~40 frames/s)')
    p.add_argument('--bw', type=int, choices=(31250, 62500), help='receive bandwidth; I/Q rate is 4x (62500 -> 250 kS/s)')
    p.add_argument('--save'); p.add_argument('--plot')
    p = sub.add_parser('sweep')
    p.add_argument('start', type=float); p.add_argument('stop', type=float); p.add_argument('step', type=float, help='Hz')
    p.add_argument('--dwell', type=int, default=1000, help='settle time per point, x16 chip loop iterations (1000 ~ 1.4 ms)')
    p.add_argument('--avg', type=int, default=4)
    p.add_argument('--out'); p.add_argument('--plot')
    a = ap.parse_args()
    s = open_port(a.port)
    if a.what == 'load':
        print(run(s, 'pram load', 10))
        return
    if a.what == 'sweep':
        text = run(s, f'pram sweep {int(a.start)} {int(a.stop)} {int(a.step)} {a.dwell} {a.avg}', 15)
        pts = []
        for line in text.splitlines():
            f, _, d = line.partition(',')
            if f.strip().isdigit() and d:
                pts.append((int(f), float(d)))
        print([l for l in text.splitlines() if 'done' in l][-1:] or text[-300:])
        if pts:
            top = sorted(pts, key=lambda p: -p[1])[:5]
            print('strongest:', ', '.join(f'{f / 1e6:.4f} MHz {d:.1f} dBm' for f, d in top))
        if a.out:
            Path(a.out).write_text('freq_hz,rssi_dbm\n' + ''.join(f'{f},{d}\n' for f, d in pts))
        if a.plot and pts:
            import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
            plt.figure(figsize=(11, 4)); plt.plot([f / 1e6 for f, _ in pts], [d for _, d in pts], lw=0.8)
            plt.xlabel('MHz'); plt.ylabel('RSSI (dBm)'); plt.grid(alpha=.3); plt.tight_layout(); plt.savefig(a.plot, dpi=100)
        return
    import numpy as np
    rows, times, chip, rate, freq, lost, done = waterfall(s, a.nseg, a.seconds, a.overlap, a.baud, a.bw)
    if not rows:
        raise SystemExit(f'no frames ({done})')
    db = np.fft.fftshift(np.frombuffer(b''.join(rows), np.uint8).reshape(-1, 256).astype(float) / 2, axes=1)
    t = np.array(times) - times[0]
    span = t[-1] if len(t) > 1 else 0
    print(f'{len(rows)} frames in {span:.2f} s = {(len(rows) - 1) / span if span else 0:.1f} frames/s, '
          f'chip {np.median(chip):.2f} ms per frame, {lost} gaps; {done}')
    off = np.fft.fftshift(np.fft.fftfreq(256, 1 / rate))
    if a.save:
        np.savez(a.save, rows=db, times=t, freq=off + freq, rate=rate)
    if a.plot:
        import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
        plt.figure(figsize=(9, 6))
        plt.pcolormesh(off / 1e3, t, db, shading='nearest', cmap='magma')
        plt.gca().invert_yaxis()
        plt.xlabel(f'offset from {freq / 1e6:.4f} MHz (kHz)'); plt.ylabel('time (s)')
        plt.title(f'Computed on the LR2021: {len(rows) / max(span, 1e-9):.0f} frames/s')
        plt.colorbar(label='dB'); plt.tight_layout(); plt.savefig(a.plot, dpi=100)


if __name__ == '__main__':
    main()
