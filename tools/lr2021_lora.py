# SPDX-License-Identifier: MIT
import argparse, json, os, signal, sys, time
from collections import deque
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
RE_TOOLS = HERE.parent / 'third_party' / 'lr2021-re' / 'tools'
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(RE_TOOLS))
import numpy as np
from lr2021_bridge import Bridge, BridgeError, CAP_GAIN_READBACK, CS12, CS16, EBUSY, IQ8, IQ6G

ROUTES = {'source2': 0, 'source0': 1, 'source1': 2, 'source6': 3}


class Stop:
    def __enter__(self):
        self.requested = False
        self.previous = signal.signal(signal.SIGINT, self._stop)
        return self

    def _stop(self, signum, frame):
        self.requested = True

    def __exit__(self, *args):
        signal.signal(signal.SIGINT, self.previous)


def to_file_bytes(h, raw):
    f, n = h['format'], h['pairs']
    if f == CS16:
        return raw[:4 * n], 'iq16'
    if f == CS12:
        b = np.frombuffer(raw, np.uint8, 3 * n).reshape(n, 3).astype(np.int32)
        w = b[:, 0] | b[:, 1] << 8 | b[:, 2] << 16
        i = ((w & 0xFFF) ^ 0x800) - 0x800
        q = ((w >> 12 & 0xFFF) ^ 0x800) - 0x800
        out = np.empty(2 * n, '<i2')
        out[0::2] = np.clip(i << h['shift'], -32768, 32767)
        out[1::2] = np.clip(q << h['shift'], -32768, 32767)
        return out.tobytes(), 'iq16'
    b = np.frombuffer(raw, np.int8, 2 * n)
    if f == IQ6G:
        b = (b.astype(np.int16) >> 2).astype(np.int8)
    return b.tobytes(), 'iq8'


def as_complex(data, sample_format):
    v = np.frombuffer(data, '<i2' if sample_format == 'iq16' else np.int8).astype(np.float32)
    return v[0::2] + 1j * v[1::2]


def configure(a):
    b = Bridge.open(a.device, a.baud)
    route = ROUTES[a.route]
    bw = int(a.bandwidth * 1000)
    if route:
        errors, rate = b.set_rx(a.freq, 0, a.gain, a.boost, a.path, route, a.code, a.rate)
    else:
        errors, rate = b.set_rx(a.freq, bw, a.gain, a.boost, a.path)
    radio = dict(frequency_hz=a.freq, bandwidth_hz=bw, sf=a.sf, route=a.route, input_code=a.code,
                 gain=a.gain, boost=a.boost, radio_errors=errors, rate_hz=rate)
    return b, rate, radio


def stream_frames(b, a, fmt, stop, on_frame):
    b.rx_start(int(a.seconds * 1000) if a.seconds else 0, fmt)
    started = time.monotonic()
    stop_sent = False
    while True:
        if stop.requested and not stop_sent:
            print('\nStopping...', flush=True)
            b.call(0x04)
            stop_sent = True
        got = b.read(0.3)
        if got is None:
            if b.ends:
                break
            continue
        if on_frame(*got) is False and not stop_sent:
            b.call(0x04)
            stop_sent = True
    return started, b.wait_end(10)


def capture(argv=None):
    a = parser('capture').parse_args(argv)
    directory = Path(a.out) / datetime.now().strftime('%Y%m%d_%H%M%S_%f')
    directory.mkdir(parents=True)
    b, rate, radio = configure(a)
    fmt = CS12 if a.cs12 else CS16
    summary = dict(valid=False, utc=datetime.now(timezone.utc).isoformat(), device=a.device, radio=radio,
                   sample_rate_hz=rate, requested_seconds=a.seconds, hello=b.hello_info)
    state = dict(next=None, samples=0, frames=0, gaps=0, gap_pairs=0, sample_format=None)
    decoder = None
    with (directory / 'iq.bin').open('wb') as iq, (directory / 'frames.jsonl').open('w') as log, Stop() as stop:
        def frame(h, raw):
            data, sf = to_file_bytes(h, raw)
            if state['sample_format'] is None:
                state['sample_format'] = sf
                summary['sample_format'] = sf
                if a.decode:
                    nonlocal decoder
                    from decode_continuous import FileDecoder, print_packet
                    decoder = FileDecoder(directory / 'iq.bin', rate, directory / 'packets.jsonl', print_packet,
                                          sample_format=sf)
                    decoder.start()
            if state['next'] is not None and h['first_pair'] != state['next']:
                missing = h['first_pair'] - state['next']
                state['gaps'] += 1
                state['gap_pairs'] += max(missing, 0)
                if missing > 0:
                    iq.write(bytes(missing * (4 if sf == 'iq16' else 2)))
            state['next'] = h['first_pair'] + h['pairs']
            iq.write(data)
            state['samples'] += h['pairs']
            state['frames'] += 1
            log.write(json.dumps(dict(h, host_monotonic=time.monotonic())) + '\n')
            if state['frames'] % 200 == 0:
                iq.flush()
                log.flush()
                print(f"{state['samples']:,} pairs / {state['frames']:,} frames, gaps {state['gaps']}", flush=True)
        summary['command_sent_monotonic'] = time.monotonic()
        try:
            started, end = stream_frames(b, a, fmt, stop, frame)
            summary['command_sent_monotonic'] = started
            summary['rx_end'] = end
            summary['valid'] = not state['gaps'] and not end['capture_error'] and not end['recovery_error']
        except BridgeError as e:
            summary['error'] = repr(e)
        finally:
            iq.flush()
            summary.update(samples=state['samples'] + state['gap_pairs'], frames=state['frames'],
                           gaps=state['gaps'], gap_pairs=state['gap_pairs'])
            if decoder:
                summary['decoder'] = decoder.finish()
            b.close()
    (directory / 'summary.json').write_text(json.dumps(summary, indent=2))
    print(f"{'Saved' if summary['valid'] else 'Saved with problems'}: {directory}", flush=True)
    return directory, summary


def snapshot(argv=None):
    a = parser('snapshot').parse_args(argv)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    b, rate, radio = configure(a)
    fmt = CS12 if a.cs12 else CS16
    use_gain = bool(b.hello_info['caps'] & CAP_GAIN_READBACK)
    block_pairs = a.block
    keep = int(a.pre * rate / block_pairs) + 1
    after = int(a.post * rate / block_pairs) + 1
    ring, pending = deque(maxlen=keep), []
    state = dict(baseline=[], quiet_step=0, armed=True, left=0, shots=0, buffer=b'', sf=None, block=0)

    def power(data, sf):
        z = as_complex(data, sf)
        return float(np.mean(np.abs(z) ** 2)) if len(z) else 0.0

    def save(blocks, reason):
        state['shots'] += 1
        data = b''.join(blocks)
        stem = out / f"shot_{datetime.now().strftime('%Y%m%d_%H%M%S')}_{state['shots']:03d}"
        Path(f'{stem}.{state["sf"]}').write_bytes(data)
        meta = dict(radio, sample_rate_hz=rate, sample_format=state['sf'], pairs=len(data) // (4 if state['sf'] == 'iq16' else 2),
                    reason=reason)
        if a.decode:
            from lora_decode import decode, DecodeError
            try:
                d = decode(as_complex(data, state['sf']), rate, a.sf, radio['bandwidth_hz'] or 62500)
                meta['decoded'] = dict(d, payload=d.get('payload', b'').hex()) if 'payload' in d else d
            except (DecodeError, ValueError) as e:
                meta['decode_error'] = str(e)
        Path(f'{stem}.json').write_text(json.dumps(meta, indent=2, default=str))
        print(f'snapshot {stem.name}: {reason}, {meta["pairs"]} pairs'
              + (f", CRC {'ok' if meta['decoded'].get('crc_ok') else 'bad'}, {len(meta['decoded'].get('payload', '')) // 2} bytes"
               if 'decoded' in meta else ''), flush=True)
        return state['shots'] < a.count

    def frame(h, raw):
        data, sf = to_file_bytes(h, raw)
        state['sf'] = state['sf'] or sf
        state['buffer'] += data
        size = block_pairs * (4 if sf == 'iq16' else 2)
        more = True
        while len(state['buffer']) >= size:
            blk, state['buffer'] = state['buffer'][:size], state['buffer'][size:]
            state['block'] += 1
            p = power(blk, sf)
            step = None
            if use_gain and state['block'] % a.gain_every == 0:
                try:
                    step = b.get_gain(timeout=5)['step']
                except BridgeError as e:
                    if e.status != EBUSY:
                        raise
            if state['left']:
                pending.append(blk)
                state['left'] -= 1
                if not state['left']:
                    more = save(list(pending), state['reason'])
                    pending.clear()
                    ring.clear()
                    state['armed'] = True
                continue
            ring.append(blk)
            if len(state['baseline']) < a.learn:
                state['baseline'].append(p)
                if step:
                    state['quiet_step'] = max(state['quiet_step'], step)
                continue
            base = float(np.median(state['baseline'])) or 1e-9
            reason = None
            if a.ratio and p > a.ratio * base:
                reason = f'power {p / base:.1f}x baseline'
            elif step and state['quiet_step'] and step < state['quiet_step']:
                reason = f'gain step {step} below quiet {state["quiet_step"]}'
            if reason and state['armed']:
                state['armed'] = False
                state['reason'] = reason
                pending[:] = list(ring)
                state['left'] = after
        return more

    with Stop() as stop:
        try:
            stream_frames(b, a, fmt, stop, frame)
        finally:
            b.close()
    print(f"{state['shots']} snapshots in {out}", flush=True)


def compare(argv=None):
    a = parser('compare').parse_args(argv)
    import serial
    from compare_rx import compare_packets, print_summary
    from kiss_rx import KissReceiver
    packets = []
    with serial.Serial(a.rak, 115200, timeout=.005, exclusive=True) as port:
        rak = KissReceiver(port, packets.append)
        rak.start()
        try:
            rak.configure(a.freq, int(a.bandwidth * 1000), a.sf, a.cr)
            before = rak.stats()
            args = [a.device, '--baud', str(a.baud), '--freq', str(a.freq), '--bandwidth', str(a.bandwidth),
                    '--sf', str(a.sf), '--gain', str(a.gain), '--route', a.route, '--code', str(a.code),
                    '--rate', str(a.rate), '--seconds', str(a.seconds), '--out', a.out, '--decode']
            if a.cs12:
                args.append('--cs12')
            directory, transport = capture(args)
            time.sleep(.05)
            after = rak.stats()
        finally:
            rak.close()
    (directory / 'rak.jsonl').write_text(''.join(json.dumps(p) + '\n' for p in packets))
    start = transport['command_sent_monotonic']
    finish = start + transport['samples'] / transport['sample_rate_hz']
    reference = [p for p in packets if start - .05 <= p['t'] <= finish + .05]
    software = []
    path = directory / 'packets.jsonl'
    for line in (path.read_text().splitlines() if path.exists() else []):
        p = json.loads(line)
        software.append(dict(payload=p['payload'], t_end=start + p['t_end_s']))
    bw = int(a.bandwidth * 1000)
    result = compare_packets(reference, software, [dict(retained_start=start, retained_end=finish)], a.sf, bw, a.cr)
    result['transport_valid'] = transport['valid']
    result['valid'] = transport['valid'] and not (transport.get('decoder') or {}).get('error')
    result['rak_hardware_counter_delta'] = {k: (after[k] - before[k]) % (1 << 32) for k in before}
    (directory / 'comparison.json').write_text(json.dumps(result, indent=2))
    print_summary(result)
    print(f'Comparison: {directory}/comparison.json', flush=True)


def parser(cmd):
    p = argparse.ArgumentParser(prog=f'lr2021_lora.py {cmd}',
                                description='LoRa software receiver on LR2021 raw IQ over the bridge protocol')
    p.add_argument('device', help='tcp:HOST[:PORT] or a serial port such as COM5 or /dev/ttyACM0')
    p.add_argument('--baud', type=int, default=4000000)
    p.add_argument('--freq', type=int, default=910525000)
    p.add_argument('--bandwidth', type=float, default=62.5, help='LoRa bandwidth in kHz (source 2 route)')
    p.add_argument('--sf', type=int, default=7)
    p.add_argument('--gain', type=int, default=0, help='0 = chip AGC, 1..13 forced')
    p.add_argument('--boost', type=int, default=0)
    p.add_argument('--path', type=int, default=2)
    p.add_argument('--route', choices=sorted(ROUTES), default='source2')
    p.add_argument('--code', type=int, default=17, help='FSK RX filter code for sources 0, 1 and 6')
    p.add_argument('--rate', type=int, default=250000, help='output rate for sources 0 and 6')
    p.add_argument('--cs12', action='store_true', help='CS12 for source 2 on slow links')
    p.add_argument('--seconds', type=float, default=60, help='0 = until Ctrl-C')
    if cmd == 'capture':
        p.add_argument('--out', default='captures/continuous')
        p.add_argument('--decode', action='store_true', help='decode LoRa packets while recording')
    if cmd == 'snapshot':
        p.add_argument('--out', default='captures/snapshots')
        p.add_argument('--ratio', type=float, default=4.0, help='trigger at this power over the quiet baseline; 0 = off')
        p.add_argument('--pre', type=float, default=0.1, help='seconds kept before the trigger')
        p.add_argument('--post', type=float, default=0.8, help='seconds kept after the trigger')
        p.add_argument('--block', type=int, default=1024, help='pairs per trigger block')
        p.add_argument('--learn', type=int, default=32, help='quiet blocks for the baseline')
        p.add_argument('--gain-every', type=int, default=4, help='read the chip AGC step every N blocks')
        p.add_argument('--count', type=int, default=1, help='snapshots before stopping')
        p.add_argument('--decode', action='store_true')
    if cmd == 'compare':
        p.add_argument('--rak', required=True, help='serial port of a KISS modem running MeshCore kiss_modem')
        p.add_argument('--cr', type=int, default=5)
        p.add_argument('--out', default='captures/compare')
    return p


def main():
    cmds = dict(capture=capture, snapshot=snapshot, compare=compare)
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        sys.exit('usage: lr2021_lora.py capture|snapshot|compare DEVICE [options]; -h after the command for help')
    cmds[sys.argv[1]](sys.argv[2:])


if __name__ == '__main__':
    main()
