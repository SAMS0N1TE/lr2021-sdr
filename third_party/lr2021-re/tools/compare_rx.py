"""Compare the LR2021 software decoder with the RAK hardware LoRa modem over KISS.

Receive-only: no KISS DATA frames or RF transmissions are sent. Results are
relative reception measurements, not absolute PER (transmissions missed by both
receivers are unknown). Stop other readers of both serial ports before running.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import re
import threading
import time

import numpy as np
import serial
from kiss_rx import KissReceiver
from live_decode import one_capture, read_until
from lora_decode import decode, DecodeError
from radio_ports import default_port


def airtime(payload_len, sf=7, bw=62500, cr=5):
    """Explicit LoRa header, payload CRC on, MeshCore's 32/16-symbol preamble."""
    symbol = (1 << sf) / bw
    de = int(symbol >= .016)
    payload_symbols = 8 + max(math.ceil((8 * payload_len - 4 * sf + 28 + 16)
                                       / (4 * (sf - 2 * de))) * cr, 0)
    return ((32 if sf <= 8 else 16) + 4.25 + payload_symbols) * symbol


def compare_packets(rak, lr, captures, sf=7, bw=62500, cr=5, guard=.03, tolerance=.05):
    """One-to-one matching of individual RF receptions, including repeated payloads.

    Only RAK packets conservatively inside a retained IQ interval enter the
    LR-vs-RAK miss denominator. Everything outside is reported separately.
    """
    candidates = sorted((abs(r['t'] - l['t_end']), ri, li)
                        for ri, r in enumerate(rak) for li, l in enumerate(lr)
                        if r['payload'] == l['payload']
                        and abs(r['t'] - l['t_end']) <= tolerance)
    paired_rak, paired_lr = {}, set()
    for _, ri, li in candidates:
        if ri not in paired_rak and li not in paired_lr:
            paired_rak[ri] = li
            paired_lr.add(li)
    eligible, missed, excluded, matched_excluded, discarded, gaps = 0, 0, 0, 0, 0, 0
    details = []
    for ri, packet in enumerate(rak):
        end = packet['t']
        start = end - airtime(len(bytes.fromhex(packet['payload'])), sf, bw, cr)
        covered = any(start >= c['retained_start'] + guard
                      and end <= c['retained_end'] - guard for c in captures)
        match = ri in paired_rak
        if covered:
            eligible += 1
            missed += not match
            status = 'both' if match else 'rak_only_retained_iq'
        elif match:
            matched_excluded += 1
            status = 'both_near_iq_boundary'
        else:
            excluded += 1
            status = 'rak_only_outside_retained_iq'
            recorded = any(start >= c.get('recording_start', c['retained_start']) + guard
                           and end <= c.get('recording_end', c['retained_end']) - guard for c in captures)
            if recorded:
                discarded += 1
            else:
                gaps += 1
        details.append(dict(rak_index=ri, lr_index=paired_rak.get(ri), status=status,
                            t_end=end, rssi_dbm=packet.get('rssi_dbm'), snr_db=packet.get('snr_db'),
                            end_delta_s=(end - lr[paired_rak[ri]]['t_end'] if match else None)))
    lr_only = len(lr) - len(paired_lr)
    offsets = [d['end_delta_s'] for d in details if d['end_delta_s'] is not None]
    return dict(rak_packets=len(rak), lr_crc_valid_packets=len(lr), matched=len(paired_rak),
                lr_only=lr_only, rak_only_retained_iq=missed,
                rak_only_outside_retained_iq=excluded, matched_near_iq_boundary=matched_excluded,
                rak_only_recorded_but_not_retained=discarded, rak_only_gap_or_partial_recording=gaps,
                rak_reference_packets_in_retained_iq=eligible,
                lr_miss_fraction_relative_to_rak=(missed / eligible if eligible else None),
                rak_miss_fraction_relative_to_lr=(lr_only / len(lr) if lr else None),
                timing_guard_s=guard, match_tolerance_s=tolerance, packets=details,
                matched_end_offset_s=(dict(min=min(offsets), median=float(np.median(offsets)), max=max(offsets))
                                      if offsets else None),
                interpretation='Conditional observed miss fractions, not absolute PER. '
                'RAK-only packets outside retained IQ are excluded from decoder misses. '
                'USB host timestamps are approximate; packets missed by both are invisible.')


def decode_capture(data, fs, sf, bw, max_attempts=8):
    """Try all non-overlapping packet regions; the capture can contain multiple packets."""
    b = np.frombuffer(data, dtype=np.int8).astype(np.float32)
    iq = b[0::2] + 1j * b[1::2]
    regions, attempts = [(0, len(iq))], []
    minimum = int(20 * (1 << sf) / bw * fs)
    while regions and len(attempts) < max_attempts:
        a, b = regions.pop(0)
        if b - a < minimum:
            continue
        try:
            result = decode(iq[a:b], fs, sf, bw)
        except DecodeError as e:
            attempts.append(dict(error=str(e), region_start_s=a / fs))
            continue
        # Strip bytes for JSON while preserving enough context to inspect failures.
        record = {k: v for k, v in result.items() if k != 'payload'}
        if 'payload' in result:
            record['payload'] = result['payload'].hex()
        attempts.append(record)
        if 'packet_end_s' not in result:
            continue
        start = max(a, a + int(result['packet_start_s'] * fs))
        end = min(b, a + math.ceil(result['packet_end_s'] * fs))
        record['packet_start_s'] += a / fs
        record['packet_end_s'] += a / fs
        if start - a >= minimum:
            regions.append((a, start))
        if b - end >= minimum and end > a:
            regions.append((end, b))
    return attempts


def summarize(events):
    settings = next(e for e in events if e['kind'] == 'settings')
    begin = next(e['t'] for e in events if e['kind'] == 'start')
    finish = next(e['t'] for e in reversed(events) if e['kind'] == 'end')
    rak = [e for e in events if e['kind'] == 'rak' and begin <= e['t'] <= finish]
    lr = [e for e in events if e['kind'] == 'lr' and begin <= e['t_end'] <= finish]
    captures = [e for e in events if e['kind'] == 'capture']
    result = compare_packets(rak, lr, captures, settings['sf'], settings['bw'], settings['cr'],
                             settings['guard_s'], settings['match_s'])
    result.update(duration_s=finish - begin, captures=len(captures),
                  retained_iq_s=sum(c['retained_end'] - c['retained_start'] for c in captures),
                  recording_s=sum(c['recording_end'] - c['recording_start'] for c in captures),
                  crc_failed_decodes=sum(a.get('crc_ok') is False for c in captures for a in c.get('attempts', [])),
                  decoder_exceptions=sum('decoder_error' in c for c in captures))
    if settings.get('invalid_reason'):
        result.update(valid=False, invalid_reason=settings['invalid_reason'],
                      lr_miss_fraction_relative_to_rak=None, rak_miss_fraction_relative_to_lr=None,
                      interpretation='INVALID RUN: ' + settings['invalid_reason'])
    before = next((e['stats'] for e in events if e['kind'] == 'stats_start'), None)
    after = next((e['stats'] for e in reversed(events) if e['kind'] == 'stats_end'), None)
    if before is not None and after is not None:
        delta = {k: (after[k] - before[k]) % (1 << 32) for k in before}
        result['rak_hardware_counter_delta'] = delta
        denominator = delta['rx'] + delta['read_errors']
        result['rak_detected_read_error_fraction'] = (delta['read_errors'] / denominator if denominator else None)
        result['rak_counter_note'] = 'Read errors are not a count of all missed transmissions or exclusively CRC failures.'
    return result


def print_summary(result):
    print(f"\nRAK modem: {result['rak_packets']} packets; LR2021 software: {result['lr_crc_valid_packets']} CRC-valid packets")
    print(f"Both: {result['matched']}; LR-only: {result['lr_only']}; "
          f"RAK-only inside retained IQ: {result['rak_only_retained_iq']}; "
          f"RAK-only outside retained IQ: {result['rak_only_outside_retained_iq']}")
    for label, key, count in [
        ('LR miss fraction relative to RAK (retained IQ only)', 'lr_miss_fraction_relative_to_rak',
         result['rak_reference_packets_in_retained_iq']),
        ('RAK miss fraction relative to LR', 'rak_miss_fraction_relative_to_lr', result['lr_crc_valid_packets']),
    ]:
        value = result[key]
        print(f'{label}: ' + (f'{value:.1%} (n={count})' if value is not None else 'no eligible packets'))
    print(result['interpretation'])
    if 'rak_hardware_counter_delta' in result:
        print('RAK hardware counter delta:', result['rak_hardware_counter_delta'])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--seconds', type=float, default=120)
    ap.add_argument('--rx', default=default_port('XIAO', '/dev/ttyACM0'))
    ap.add_argument('--rak', default=default_port('RAK3401', '/dev/ttyACM1'))
    ap.add_argument('--freq', type=float, default=910525000)
    ap.add_argument('--bw', type=float, default=62500)
    ap.add_argument('--sf', type=int, choices=range(5, 13), default=7)
    ap.add_argument('--cr', type=int, choices=range(5, 9), default=5)
    ap.add_argument('--gain', type=int, choices=range(14), default=0)
    ap.add_argument('--window', type=float, default=8)
    ap.add_argument('--guard-ms', type=float, default=30)
    ap.add_argument('--match-ms', type=float, default=50)
    ap.add_argument('--out', default='captures/per')
    ap.add_argument('--report', type=Path, help='recompute an existing events.jsonl without opening radios')
    args = ap.parse_args()
    if args.report:
        print_summary(summarize([json.loads(line) for line in args.report.read_text().splitlines()]))
        return
    if args.seconds <= 0 or args.window <= 0 or not 0 < args.bw <= 250000 / 3:
        ap.error('seconds/window must be positive; bandwidth must fit the 83.333 kS/s capture')
    if args.guard_ms < 0 or args.match_ms <= 0:
        ap.error('guard must be nonnegative and match tolerance positive')
    directory = Path(args.out) / datetime.now().strftime('%Y%m%d_%H%M%S_%f')
    directory.mkdir(parents=True)
    events, lock = [], threading.Lock()
    with (directory / 'events.jsonl').open('w') as log:
        def record(event):
            with lock:
                events.append(event)
                log.write(json.dumps(event) + '\n')
                log.flush()

        record(dict(kind='settings', utc=datetime.now(timezone.utc).isoformat(),
                    sf=args.sf, bw=args.bw, cr=args.cr, frequency_hz=args.freq,
                    gain=args.gain, guard_s=args.guard_ms / 1000, match_s=args.match_ms / 1000,
                    kiss_keepalive_ms=10, kiss_read_timeout_ms=5))
        with serial.Serial(args.rx, 115200, timeout=.2, exclusive=True) as rx, \
                serial.Serial(args.rak, 115200, timeout=.005, exclusive=True) as rak_port:
            def got_rak(packet):
                record(dict(packet, kind='rak'))
                print(f"RAK RX {len(packet['payload']) // 2}B RSSI={packet.get('rssi_dbm')} SNR={packet.get('snr_db')}", flush=True)

            rak = KissReceiver(rak_port, got_rak)
            rak.start()
            try:
                rak.configure(args.freq, args.bw, args.sf, args.cr)
                rx.write(b'hold on\r')
                if b'hold st=0 on' not in read_until(rx, b'hold st=0 on', 3):
                    raise RuntimeError('LR2021 did not enter hold mode')
                rx.write(b'get radio\r')
                config_reply = read_until(rx, b'  -> > ', 3).decode(errors='replace')
                config = re.search(r'>\s+([\d.]+),([\d.]+),(\d+),(\d+)', config_reply)
                if not config or abs(float(config[1]) * 1e6 - args.freq) > 100 or \
                        abs(float(config[2]) * 1e3 - args.bw) > 1 or int(config[3]) != args.sf or int(config[4]) != args.cr:
                    raise RuntimeError(f'LR2021 radio settings differ from comparison settings: {config_reply!r}')
                rx.write(f'rc 021A 0 {args.gain:02X}\r'.encode())
                if b'rc st=0' not in read_until(rx, b'rc st=0', 3):
                    raise RuntimeError('LR2021 gain configuration failed')
                record(dict(kind='stats_start', stats=rak.stats()))
                begin = time.monotonic()
                record(dict(kind='start', t=begin))
                print(f'Listening on both radios for {args.seconds:g}s; saving {directory}', flush=True)
                futures = []
                with ThreadPoolExecutor(max_workers=1) as decoder:
                    try:
                        while time.monotonic() - begin < args.seconds:
                            if rak.error:
                                raise RuntimeError(f'KISS receiver stopped: {rak.error}')
                            timing = {}
                            summary, data = one_capture(rx, f'stream 0 3 {int(args.window * 1000)} 8 8 3', args.window, timing)
                            if not data:
                                raise RuntimeError(f'no LR2021 IQ: {summary}')
                            fields = dict(re.findall(r'(\w+)=([^\s]+)', summary or ''))
                            if 'ms' not in fields:
                                raise RuntimeError(f'missing capture timing: {summary}')
                            number = len(futures)
                            path = directory / f'cap_{number:05d}.bin'
                            path.write_bytes(data)
                            end = timing['command_sent'] + int(fields['ms']) / 1000
                            capture = dict(kind='capture', id=number, file=str(path), summary=summary,
                                           recording_start=timing['command_sent'], recording_end=end,
                                           retained_start=max(timing['command_sent'], end - len(data) / (2 * (250000 / 3))),
                                           retained_end=end, timing=timing)
                            # Re-arm without waiting for software decoding to finish.
                            def process(c=capture, iq=data):
                                try:
                                    attempts = decode_capture(iq, 250000 / 3, args.sf, args.bw)
                                    c['attempts'] = attempts
                                    for a in attempts:
                                        if a.get('crc_ok') is True:
                                            record(dict(kind='lr', capture_id=c['id'], payload=a['payload'],
                                                        t_start=c['retained_start'] + a['packet_start_s'],
                                                        t_end=c['retained_start'] + a['packet_end_s'],
                                                        triggered='hit=1' in c['summary']))
                                            print(f"LR  RX {len(a['payload']) // 2}B CRC OK (capture {c['id']})", flush=True)
                                except Exception as e:
                                    c['decoder_error'] = repr(e)
                                record(c)
                            futures.append(decoder.submit(process))
                            print(f"Capture {number}: {fields.get('trigger', fields.get('hit'))}", flush=True)
                    except KeyboardInterrupt:
                        print('\nStopping comparison...', flush=True)
                    finally:
                        record(dict(kind='end', t=time.monotonic()))
                for future in futures:
                    future.result()
                time.sleep(.1)  # drain a final packet's metadata
                record(dict(kind='stats_end', stats=rak.stats()))
            finally:
                rak.close()
        result = summarize(events)
        (directory / 'summary.json').write_text(json.dumps(result, indent=2))
        print_summary(result)
        print(f'Results: {directory}/summary.json', flush=True)


if __name__ == '__main__':
    main()
