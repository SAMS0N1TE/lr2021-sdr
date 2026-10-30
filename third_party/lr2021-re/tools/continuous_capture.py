"""Record checked continuous LR2021 IQ. Requires iqstream debug firmware.

Native USB CDC; the baud setting is not a UART throughput limit. Ctrl-C requests
a clean stop. The receiver stays held in raw mode afterwards; reboot to resume MeshCore.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import signal
import time

import serial
from iq_stream import IQStream, START, DATA, END
from radio_ports import default_port


class StopSignal:
    """Finish processing a received batch before requesting the device to stop."""
    def __enter__(self):
        self.requested = False
        self.previous = signal.signal(signal.SIGINT, self._stop)
        return self

    def _stop(self, signum, frame):
        self.requested = True

    def __exit__(self, *args):
        signal.signal(signal.SIGINT, self.previous)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--port', default=default_port('XIAO', '/dev/ttyACM0'))
    ap.add_argument('--seconds', type=float, default=60, help='0=until Ctrl-C')
    ap.add_argument('--spi', type=int, choices=(8, 16, 32), default=16, help='32 MHz is experimental radio overclocking')
    ap.add_argument('--rx-delay', type=int, choices=range(8), default=0, help='experimental 32 MHz MISO sampling delay in 15.625 ns steps')
    ap.add_argument('--dec', type=int, choices=range(1, 9), default=3)
    ap.add_argument('--format', choices=('iq8', 'iq16'), default='iq8', help='native source 0 IQ8 or source 2 little-endian IQ16')
    ap.add_argument('--bandwidth', type=float, choices=(31.25, 62.5, 125, 250), help='IQ16 capture bandwidth override in kHz; does not save radio preferences')
    ap.add_argument('--gain', type=int, choices=range(14), default=0)
    ap.add_argument('--out', default='captures/continuous')
    ap.add_argument('--decode', action='store_true', help='follow the IQ file and print CRC-valid MeshCore packets')
    ap.add_argument('--pause-reader', type=float, default=0,
                    help='test only: pause once after the first IQ frame (seconds)')
    args = ap.parse_args(argv)
    if args.seconds < 0 or args.pause_reader < 0:
        ap.error('duration and reader pause must be nonnegative')
    if args.rx_delay and args.spi != 32:
        ap.error('--rx-delay is only supported with --spi 32')
    if args.format == 'iq8' and args.bandwidth not in (None, 62.5):
        ap.error('--bandwidth overrides other than 62.5 require --format iq16')
    if args.decode and args.bandwidth not in (None, 62.5):
        ap.error('--decode currently requires BW62.5')
    directory = Path(args.out) / datetime.now().strftime('%Y%m%d_%H%M%S_%f')
    directory.mkdir(parents=True)
    summary = dict(valid=False, utc=datetime.now(timezone.utc).isoformat(), port=args.port,
                   requested_seconds=args.seconds, spi_mhz=args.spi, decimation=args.dec, gain=args.gain,
                   sample_format=args.format, bandwidth_override_khz=args.bandwidth, rx_delay=args.rx_delay,
                   rx_delay_applied=args.rx_delay if args.spi == 32 else None)
    failure = None
    decoder = None
    started = progress = time.monotonic()
    with (directory / 'iq.bin').open('wb') as iq, (directory / 'frames.jsonl').open('w') as log, StopSignal() as stop:
        with serial.Serial(args.port, 115200, timeout=.02, write_timeout=2, exclusive=True) as port:
            stream = IQStream(port)
            try:
                stream.start(args.seconds, args.spi, args.dec, args.gain, args.format, args.bandwidth, args.rx_delay)
                summary['radio'] = stream.radio
                summary['command_sent_monotonic'] = stream.command_sent
                if args.decode:
                    if stream.radio['sf'] != 7 or stream.radio['bandwidth_hz'] != 62500:
                        raise ValueError('live file decoder currently supports SF7/BW62.5 only; record without --decode')
                    from decode_continuous import FileDecoder, print_packet
                    decoder = FileDecoder(directory / 'iq.bin', stream.raw_rate / args.dec,
                                          directory / 'packets.jsonl', print_packet, sample_format=args.format)
                    decoder.start()
                print(f'Continuous RX -> {directory}/iq.bin; Ctrl-C to stop', flush=True)
                paused = False
                while not stream.validator.ended:
                    if stop.requested and not stream.stop_sent:
                        print('\nRequesting a clean stop...', flush=True)
                        stream.request_stop()
                    try:
                        frames = stream.poll()
                    except KeyboardInterrupt:
                        print('\nRequesting a clean stop...', flush=True)
                        stream.request_stop()
                        continue
                    for frame in frames:
                        entry = dict(frame.metadata(), host_monotonic=time.monotonic())
                        if frame.kind in (START, END):
                            entry['stats'] = frame.stats()
                        log.write(json.dumps(entry) + '\n')
                        # Log terminal errors before validation raises.
                        stream.validator.accept(frame)
                        if frame.kind == DATA:
                            iq.write(frame.payload)
                            if args.pause_reader and not paused:
                                paused = True
                                time.sleep(args.pause_reader)
                    now = time.monotonic()
                    if now - progress >= 1:
                        iq.flush()
                        log.flush()
                        print(f'{stream.validator.samples:,} pairs / {stream.validator.data_frames:,} frames', flush=True)
                        progress = now
                summary['valid'] = True
            except Exception as e:
                failure = e
                summary['error'] = repr(e)
                try:
                    stream.request_stop()
                    # Keep reading briefly so the device can drain/return to its held CLI.
                    until = time.monotonic() + 2
                    while time.monotonic() < until:
                        port.read(16384)
                except Exception as stop_error:
                    summary['stop_error'] = repr(stop_error)
            finally:
                summary.update(samples=stream.validator.samples, frames=stream.validator.data_frames,
                               terminal_flags=stream.validator.final_flags,
                               device=stream.validator.final_stats, host_elapsed_s=time.monotonic() - started)
                if stream.validator.raw_rate:
                    summary['sample_rate_hz'] = stream.validator.raw_rate / stream.validator.decimation
                iq.flush()
                if decoder:
                    summary['decoder'] = decoder.finish()
    (directory / 'summary.json').write_text(json.dumps(summary, indent=2))
    if failure:
        print(f'FAILED: {failure}\nReport: {directory}/summary.json', flush=True)
        raise SystemExit(1)
    print('Continuity checks passed:', json.dumps(summary['device']), flush=True)
    if (summary.get('decoder') or {}).get('error'):
        print(f"Packet decoder failed; checked IQ is saved: {summary['decoder']['error']}", flush=True)
    print(f'Saved {directory}', flush=True)
    return directory, summary


if __name__ == '__main__':
    main()
