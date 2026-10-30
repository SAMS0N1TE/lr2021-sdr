"""Checked continuous IQ transport for the LR2021 debug firmware (LRIQ v1 IQ8 / v2 IQ16)."""
from dataclasses import dataclass
import re
import struct
import time
import zlib

from live_decode import read_until

HEADER = struct.Struct('<4sBBHIIQIIHHIII')
STATS = struct.Struct('<QQQQIIIIIIIi')
START, DATA, END = 1, 2, 3
FIFO_LOSS, QUEUE_FULL, STOPPED, DURATION, BUSY_TIMEOUT, USB_STALL, CONFIG_ERROR = 1, 2, 4, 8, 16, 32, 64
ERROR_FLAGS = FIFO_LOSS | QUEUE_FULL | BUSY_TIMEOUT | USB_STALL | CONFIG_ERROR
STAT_NAMES = ('raw_samples', 'produced_samples', 'sent_samples', 'elapsed_us', 'max_fifo',
              'fifo_flags', 'max_service_gap_us', 'max_queue_bytes', 'queue_overflows',
              'busy_timeouts', 'spi_hz', 'config_error')


class StreamError(ValueError):
    pass


@dataclass
class Frame:
    kind: int
    generation: int
    sequence: int
    first_sample: int
    sample_count: int
    raw_rate: int
    decimation: int
    flags: int
    payload_crc: int
    payload: bytes
    version: int = 1

    @property
    def pair_bytes(self):
        return 4 if self.version == 2 else 2

    def metadata(self):
        return {k: v for k, v in self.__dict__.items() if k != 'payload'}

    def stats(self):
        if self.kind not in (START, END) or len(self.payload) != STATS.size:
            raise StreamError('invalid stream status payload')
        return dict(zip(STAT_NAMES, STATS.unpack(self.payload)))


class FrameParser:
    def __init__(self):
        self.buffer = bytearray()
        self.synchronized = False
        self.prefix = bytearray()

    def feed(self, data):
        self.buffer.extend(data)
        frames = []
        while True:
            if not self.synchronized:
                offset = self.buffer.find(b'LRIQ')
                if offset < 0:
                    n = max(0, len(self.buffer) - 3)
                    self.prefix.extend(self.buffer[:n])
                    del self.buffer[:n]
                    if len(self.prefix) > 4096:
                        raise StreamError('no LRIQ stream header; check debug firmware')
                    return frames
                self.prefix.extend(self.buffer[:offset])
                del self.buffer[:offset]
                self.synchronized = True
            if len(self.buffer) < HEADER.size:
                return frames
            header = bytes(self.buffer[:HEADER.size])
            values = HEADER.unpack(header)
            (magic, version, kind, size, gen, seq, first, count, rate, dec, flags,
             length, crc, header_crc) = values
            if magic != b'LRIQ' or version not in (1, 2) or size != HEADER.size:
                raise StreamError('invalid LRIQ framing/version; continuity lost')
            if zlib.crc32(header[:-4]) != header_crc:
                raise StreamError('stream header checksum mismatch')
            if kind not in (START, DATA, END) or length > 1024:
                raise StreamError('invalid stream frame type/length')
            if len(self.buffer) < size + length:
                return frames
            payload = bytes(self.buffer[size:size + length])
            if zlib.crc32(payload) != crc:
                raise StreamError('stream payload checksum mismatch')
            del self.buffer[:size + length]
            frames.append(Frame(kind, gen, seq, first, count, rate, dec, flags, crc, payload, version))
            if kind == END:
                # A following CLI newline is outside the binary stream.
                return frames


class StreamValidator:
    def __init__(self):
        self.started = self.ended = False
        self.generation = None
        self.sequence = self.samples = self.data_frames = 0
        self.raw_rate = self.decimation = self.pair_bytes = None
        self.final_stats = None
        self.final_flags = None

    def accept(self, frame):
        if self.ended:
            raise StreamError('frame after END')
        if not self.started:
            if frame.kind != START or frame.sequence != 0 or frame.first_sample or frame.sample_count:
                raise StreamError('stream must begin with START at sample zero')
            if frame.raw_rate <= 0 or not 1 <= frame.decimation <= 8:
                raise StreamError('invalid sample rate/decimation')
            frame.stats()
            self.generation = frame.generation
            self.raw_rate, self.decimation = frame.raw_rate, frame.decimation
            self.pair_bytes = frame.pair_bytes
            self.started = True
        elif frame.kind == START:
            raise StreamError('unexpected new stream generation')
        if frame.generation != self.generation or frame.sequence != self.sequence:
            raise StreamError('stream generation/sequence discontinuity')
        if frame.first_sample != self.samples:
            raise StreamError(f'sample discontinuity: expected {self.samples}, got {frame.first_sample}')
        if (frame.raw_rate, frame.decimation, frame.pair_bytes) != (self.raw_rate, self.decimation, self.pair_bytes):
            raise StreamError('sample format changed during stream')
        self.sequence = (self.sequence + 1) & 0xffffffff
        if frame.kind == DATA:
            if frame.flags or not frame.sample_count or frame.sample_count * frame.pair_bytes != len(frame.payload):
                raise StreamError('invalid IQ data frame')
            self.samples += frame.sample_count
            self.data_frames += 1
        elif frame.kind == END:
            self.final_stats = stats = frame.stats()
            self.final_flags = frame.flags
            self.ended = True
            if frame.sample_count or not frame.flags & (STOPPED | DURATION | ERROR_FLAGS):
                raise StreamError('invalid END reason/count')
            if frame.flags & ERROR_FLAGS:
                raise StreamError(f'device ended with error flags 0x{frame.flags:02x}: {stats}')
            if stats['fifo_flags'] & ~0x0f or stats['queue_overflows'] or stats['busy_timeouts'] or stats['config_error']:
                raise StreamError(f'device reported capture loss/error: {stats}')
            if stats['sent_samples'] != self.samples or stats['produced_samples'] != self.samples:
                raise StreamError('device/host sample totals differ')
            if stats['elapsed_us'] >= 100000:
                expected_raw = self.raw_rate * stats['elapsed_us'] / 1e6
                if abs(stats['raw_samples'] / expected_raw - 1) > .02:
                    raise StreamError('measured raw sample rate differs from nominal by more than 2%; suspect SPI corruption')
            if stats['raw_samples'] // self.decimation != self.samples:
                raise StreamError('decimation sample totals differ')
        elif frame.flags:
            raise StreamError(f'device configuration failed: {frame.stats()}')


class IQStream:
    def __init__(self, port):
        self.port = port
        self.parser = FrameParser()
        self.validator = StreamValidator()
        self.radio = None
        self.last_frame_at = self.last_ping_at = time.monotonic()
        self.stop_sent = False

    def start(self, seconds=0, spi_mhz=16, decimation=3, gain=0, sample_format='iq8', bandwidth=None, rx_delay=0):
        if seconds < 0 or spi_mhz not in (8, 16, 32) or decimation not in range(1, 9) or gain not in range(14):
            raise ValueError('invalid stream duration/SPI/decimation/gain')
        if rx_delay not in range(8) or (rx_delay and spi_mhz != 32):
            raise ValueError('RX delay 0..7 is only experimental at 32 MHz')
        if sample_format not in ('iq8', 'iq16') or bandwidth not in (None, 31.25, 62.5, 125, 250):
            raise ValueError('invalid sample format/bandwidth')
        if sample_format == 'iq8' and bandwidth not in (None, 62.5):
            raise ValueError('IQ8 currently requires BW62.5')
        self.port.reset_input_buffer()
        self.port.write(b'\rhold on\r')
        if b'hold st=0 on' not in read_until(self.port, b'hold st=0 on', 3):
            raise StreamError('receiver did not enter hold mode')
        self.port.write(b'get radio\r')
        text = read_until(self.port, b'  -> > ', 3).decode(errors='replace')
        radio = re.search(r'>\s+([\d.]+),([\d.]+),(\d+),(\d+)', text)
        if not radio:
            raise StreamError(f'cannot read radio settings: {text!r}')
        self.radio = dict(frequency_mhz=float(radio[1]), bandwidth_hz=float(radio[2]) * 1000,
                          sf=int(radio[3]), cr=int(radio[4]))
        if bandwidth is not None:
            self.radio['bandwidth_hz'] = bandwidth * 1000
        if sample_format == 'iq8' and self.radio['bandwidth_hz'] != 62500:
            raise StreamError('this source-0 transport currently requires BW62.5 (250 ksample/s raw)')
        if sample_format == 'iq16' and self.radio['bandwidth_hz'] not in (31250, 62500, 125000, 250000):
            raise StreamError('IQ16 bandwidth must be 31.25/62.5/125/250 kHz')
        self.sample_format = sample_format
        self.requested_decimation = decimation
        self.raw_rate = round(self.radio['bandwidth_hz'] * 4)
        self.port.write(f'rc 021A 0 {gain:02X}\r'.encode())
        if b'rc st=0' not in read_until(self.port, b'rc st=0', 3):
            raise StreamError('gain configuration failed')
        self.command_sent = time.monotonic()
        duration_ms = max(1, round(seconds * 1000)) if seconds else 0
        bits = 16 if sample_format == 'iq16' else 8
        self.port.write(f'iqstream {duration_ms} {spi_mhz} {decimation} {bits} {self.raw_rate} {rx_delay}\r'.encode())
        self.last_frame_at = self.last_ping_at = time.monotonic()

    def request_stop(self):
        if not self.stop_sent and not self.validator.ended:
            self.port.write(b'\x03')
            self.stop_sent = True

    def poll(self):
        now = time.monotonic()
        if now - self.last_ping_at >= .1:
            self.port.write(b'\n')
            self.last_ping_at = now
        data = self.port.read(16384)
        frames = self.parser.feed(data)
        for frame in frames:
            if frame.kind == START and (frame.pair_bytes != (4 if self.sample_format == 'iq16' else 2)
                                        or frame.raw_rate != self.raw_rate
                                        or frame.decimation != self.requested_decimation):
                raise StreamError('firmware returned a different format/rate/decimation; update firmware')
        if frames:
            self.last_frame_at = time.monotonic()
        elif time.monotonic() - self.last_frame_at > 5:
            raise StreamError(f'stream stalled/missing END; prefix={bytes(self.parser.prefix[-120:])!r}')
        return frames
