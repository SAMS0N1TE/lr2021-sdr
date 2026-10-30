"""Receive-only host for the MeshCore KISS modem in MeshCore examples/kiss_modem.

Only hardware-control frames are sent; this module never sends KISS DATA (RF TX).
"""
from collections import defaultdict, deque
import struct
import threading
import time


def encode_frame(kind, payload=b''):
    body = bytes([kind]) + payload
    return b'\xc0' + body.replace(b'\xdb', b'\xdb\xdd').replace(b'\xc0', b'\xdb\xdc') + b'\xc0'


class KissParser:
    def __init__(self):
        self.active = False
        self.escaped = False
        self.buffer = bytearray()

    def feed(self, data):
        frames = []
        for byte in data:
            if byte == 0xc0:
                if self.active and self.buffer and not self.escaped:
                    frames.append(bytes(self.buffer))
                self.buffer.clear()
                self.active = True
                self.escaped = False
            elif self.active:
                if self.escaped:
                    self.escaped = False
                    if byte not in (0xdc, 0xdd):
                        self.active = False
                        self.buffer.clear()
                        continue
                    self.buffer.append(0xc0 if byte == 0xdc else 0xdb)
                elif byte == 0xdb:
                    self.escaped = True
                else:
                    self.buffer.append(byte)
                if len(self.buffer) > 1024:
                    self.active = False
                    self.buffer.clear()
        return frames


class KissReceiver:
    def __init__(self, serial_port, on_packet):
        self.serial = serial_port
        self.on_packet = on_packet
        self.parser = KissParser()
        self.condition = threading.Condition()
        self.replies = defaultdict(lambda: deque(maxlen=16))
        self.write_lock = threading.Lock()
        self.pending = None
        self.stop = threading.Event()
        self.error = None
        self.thread = threading.Thread(target=self._read, daemon=True)

    def start(self):
        self.thread.start()

    def close(self):
        self.stop.set()
        self.thread.join(timeout=3)
        self._flush_packet()

    def _flush_packet(self):
        if self.pending is not None:
            packet, self.pending = self.pending, None
            self.on_packet(packet)

    def _frame(self, frame, now):
        kind, data = frame[0], frame[1:]
        if kind == 0 and data:
            self._flush_packet()
            self.pending = dict(t=now, payload=data.hex(), crc_ok=True)
        elif kind == 6 and data:
            sub, body = data[0], data[1:]
            if sub == 0xf9 and len(body) == 2:
                if self.pending is not None:
                    snr, rssi = struct.unpack('bb', body)
                    self.pending.update(snr_db=snr / 4, rssi_dbm=rssi)
                    self._flush_packet()
            else:
                with self.condition:
                    self.replies[sub].append(body)
                    self.condition.notify_all()

    def _read(self):
        try:
            next_poll = 0
            while not self.stop.is_set():
                now = time.monotonic()
                if now >= next_poll:
                    # Unsolicited RX on this nRF52 KISS setup can remain buffered
                    # until host traffic resumes. Hardware-only pings keep the
                    # export path moving; they never transmit over the air.
                    with self.write_lock:
                        self.serial.write(encode_frame(6, b'\x17'))
                    next_poll = now + .01
                data = self.serial.read(4096)
                now = time.monotonic()
                for frame in self.parser.feed(data):
                    self._frame(frame, now)
                if self.pending is not None and now - self.pending['t'] > .1:
                    self._flush_packet()
        except Exception as e:
            self.error = e
            with self.condition:
                self.condition.notify_all()

    def request(self, command, payload=b'', response=None, timeout=3):
        response = (command | 0x80) if response is None else response
        deadline = time.monotonic() + timeout
        with self.condition:
            self.replies[response].clear()
            self.replies[0xf1].clear()
            with self.write_lock:
                self.serial.write(encode_frame(6, bytes([command]) + payload))
            while not self.replies[response]:
                if self.error:
                    raise RuntimeError(f'KISS reader failed: {self.error}')
                if self.replies[0xf1]:
                    raise RuntimeError(f'KISS error: {self.replies[0xf1].popleft().hex()}')
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise RuntimeError(f'no KISS response to command 0x{command:02x}; check modem firmware/port')
                self.condition.wait(remaining)
            return self.replies[response].popleft()

    def configure(self, frequency, bandwidth, sf, cr):
        for attempt in range(3):
            try:
                self.request(0x17, timeout=1)  # allow USB/firmware startup to settle
                break
            except RuntimeError:
                if self.error or attempt == 2:
                    raise
        self.request(0x0a, b'\xf7', response=0xf0)  # -9 dBm; no transmission
        config = struct.pack('<IIBB', int(frequency), int(bandwidth), sf, cr)
        self.request(0x09, config, response=0xf0)
        if self.request(0x0b) != config:
            raise RuntimeError('RAK KISS radio configuration mismatch')
        self.request(0x19, b'\x01', response=0x9a)  # enable RX metadata

    def stats(self):
        raw = self.request(0x12)
        if len(raw) != 12:
            raise RuntimeError('invalid KISS stats response')
        rx, tx, errors = struct.unpack('<III', raw)
        return dict(rx=rx, tx=tx, read_errors=errors)
