# Loop: arm an LR2021 triggered IQ capture, fetch it, software-dechirp, print one line per packet.
# Needs the LR2021_DEBUG_CLI repeater firmware on the XIAO. Ctrl-C to stop; reboot the XIAO afterwards.
# Usage: live_decode.py [--port /dev/ttyACM0] [--gain 0] [--diagnostics] [--save DIR]
import argparse, os, sys, time, datetime
import serial
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lora_decode import decode, DecodeError
from meshcore_decode import summarize_packet
from radio_ports import default_port


def read_until(ser, marker, timeout):
    buf = b''
    end = time.time() + timeout
    while time.time() < end:
        d = ser.read(65536)
        if d:
            buf += d
            if marker in buf:
                break
    return buf


def one_capture(ser, cmd, window_s, timing=None):
    ser.reset_input_buffer()
    if timing is not None:
        timing['command_sent'] = time.monotonic()
    ser.write(cmd.encode() + b'\r')
    # A triggered capture finishes early. Waiting the full acquisition window
    # leaves the radio idle for seconds after every packet.
    deadline = time.monotonic() + window_s + 1.5
    completion = b''
    next_poll = time.monotonic() + .2
    while time.monotonic() < deadline:
        completion += ser.read(4096)
        if b'stream done' in completion:
            break
        if time.monotonic() >= next_poll:
            # Keep the nRF52 USB export responsive during the blocking capture.
            # Newlines are ignored by the CLI, so they cannot queue commands or TX.
            ser.write(b'\n')
            next_poll = time.monotonic() + .2
    if timing is not None:
        timing['completion_observed'] = time.monotonic()
    ser.write(b'streamdump\r')
    buf = read_until(ser, b'STREAMBIN', 20)
    if b'STREAMBIN' not in buf:
        return None, None
    i = buf.index(b'STREAMBIN')
    while b'\n' not in buf[i:]:
        d = ser.read(4096)
        if not d:
            raise ValueError('incomplete STREAMBIN header')
        buf += d
    nl = buf.index(b'\n', i)
    summary = buf[:i].decode(errors='replace').strip().split('\n')[-1]
    n = int(buf[i:nl].split()[1])
    data = buf[nl + 1:]
    old_timeout = ser.timeout
    ser.timeout = 2
    try:
        while len(data) < n:
            d = ser.read(n - len(data))
            if not d:
                raise ValueError(f'truncated IQ transfer ({len(data)} of {n} bytes)')
            data += d
    finally:
        ser.timeout = old_timeout
    if n % 2:
        raise ValueError(f'odd IQ byte count: {n}')
    return summary, data[:n]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', default=default_port('XIAO', '/dev/ttyACM0'))
    ap.add_argument('--ratio', type=int, default=3)
    ap.add_argument('--dec', type=int, default=3)
    ap.add_argument('--window', type=int, default=8, help='seconds to wait for a trigger per round')
    ap.add_argument('--prekb', type=int, default=8)
    ap.add_argument('--spi', type=int, default=8)
    ap.add_argument('--gain', type=int, choices=range(14), default=0,
                    help='raw-mode RX gain: 0=hardware AGC (default), 1..13=manual; auto needs gain-aware debug firmware')
    ap.add_argument('--diagnostics', action='store_true', help='print firmware capture/trigger status')
    ap.add_argument('--rounds', type=int, default=0, help='stop after this many captures (0=forever)')
    ap.add_argument('--sf', type=int, default=7)
    ap.add_argument('--bw', type=float, default=62500.0)
    ap.add_argument('--save', default=None)
    args = ap.parse_args()
    fs = 250000.0 / args.dec

    if args.save:
        os.makedirs(args.save, exist_ok=True)
    ser = serial.Serial(args.port, 115200, timeout=0.2, exclusive=True)
    time.sleep(0.3)
    ser.reset_input_buffer()
    ser.write(b'hold on\r')
    time.sleep(0.3)
    ser.reset_input_buffer()
    ser.write(f'rc 021A 0 {args.gain:02X}\r'.encode())
    gain_reply = read_until(ser, b'rc st=0', 2)
    if b'rc st=0' not in gain_reply:
        raise RuntimeError(f'could not configure RX gain: {gain_reply!r}')

    cmd = f'stream 0 {args.ratio} {args.window * 1000} {args.prekb} {args.spi} {args.dec}'
    print(f'# listening on {args.port}, {fs/1e3:.1f} kS/s, SF{args.sf} BW{args.bw/1e3:.0f}k, gain={args.gain}; Ctrl-C to stop', flush=True)
    print(f'# {"time":8} {"dur":>6} {"snr":>5} {"cfo":>7} {"len":>4} {"cr":>3} crc  payload', flush=True)
    rounds = 0
    while not args.rounds or rounds < args.rounds:
        rounds += 1
        try:
            summary, data = one_capture(ser, cmd, args.window)
        except ValueError as e:
            print(f'# capture error: {e}', flush=True)
            continue
        if args.diagnostics:
            print(f'# {summary}', flush=True)
        if not data:
            print('# no capture (radio not responding?)', flush=True)
            time.sleep(1)
            continue
        now = datetime.datetime.now().strftime('%H:%M:%S')
        triggered = 'hit=1' in (summary or '')
        if not triggered:
            # no signal crossed the trigger this round; show a heartbeat so the loop is visibly alive
            print(f'{now}  ... quiet (round {rounds})', end='\r', flush=True)
            continue
        if args.save:
            # Keep failed captures too, and never overwrite a previous session.
            fn = os.path.join(args.save, datetime.datetime.now().strftime('cap_%Y%m%d_%H%M%S_%f.bin'))
            with open(fn, 'xb') as f:
                f.write(data)
        try:
            import numpy as np
            b = np.frombuffer(data, dtype=np.int8).astype(float)
            iq = b[0::2] + 1j * b[1::2]
            r = decode(iq, fs, args.sf, args.bw)
        except DecodeError as e:
            print(f'{now} triggered but no decode: {e}', flush=True)
            continue
        except Exception as e:
            print(f'{now} decode error: {e}', flush=True)
            continue
        crc = 'ok' if r.get('crc_ok') else ('BAD' if 'crc_ok' in r else '--')
        pl = r.get('payload', b'')
        hexpl = pl.hex(' ')
        if len(hexpl) > 180:
            hexpl = hexpl[:180] + ' ...'
        detail = summarize_packet(r['meshcore']) if 'meshcore' in r else hexpl
        if 'meshcore_error' in r:
            detail = f"MeshCore parse error: {r['meshcore_error']}; {hexpl}"
        err = r.get('error', '')
        print(f'{now} {r.get("duration_ms",0):6.1f} {r.get("snr_db",0):5.1f} {r.get("cfo_hz",0):7.0f} '
              f'{r.get("len","?"):>4} 4/{r.get("cr","?")+4 if isinstance(r.get("cr"),int) else "?"} {crc:>3}  '
              f'{err or detail}')
    ser.close()


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print('\n# stopped', flush=True)
