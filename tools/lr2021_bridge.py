# SPDX-License-Identifier: MIT
import collections, socket, struct, time, zlib

HELLO, SET_RX, RX_START, RX_STOP, SET_GAIN, GET_TIME, BYE, GET_GAIN = 0x01, 0x02, 0x03, 0x04, 0x05, 0x09, 0x0A, 0x0B
RX_DATA, RX_END = 0xC0, 0xC1
CAP_LIVE_GAIN, CAP_CS12, CAP_RAW_ROUTES, CAP_GAIN_READBACK = 8, 32, 128, 256
CS16, CS12, IQ8, IQ6G = 0, 1, 2, 3
EBUSY = -16


class BridgeError(Exception):
    def __init__(self, what, status=None):
        super().__init__(f'{what}: status {status}' if status is not None else what)
        self.status = status


def encode(kind, seq, payload=b''):
    body = bytes([1, kind]) + struct.pack('<HH', seq, len(payload)) + payload
    return b'LR' + body + struct.pack('<I', zlib.crc32(body) & 0xFFFFFFFF)


class Parser:
    def __init__(self):
        self.buf = bytearray()
        self.crc_errors = 0

    def push(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(b'LR\x01')
            if i < 0:
                del self.buf[:max(0, len(self.buf) - 2)]
                return out
            del self.buf[:i]
            if len(self.buf) < 8:
                return out
            n = struct.unpack_from('<H', self.buf, 6)[0]
            if n > 2048:
                del self.buf[:1]
                continue
            if len(self.buf) < 12 + n:
                return out
            crc = struct.unpack_from('<I', self.buf, 8 + n)[0]
            if zlib.crc32(bytes(self.buf[2:8 + n])) & 0xFFFFFFFF != crc:
                self.crc_errors += 1
                del self.buf[:1]
                continue
            kind = self.buf[3]
            seq = struct.unpack_from('<H', self.buf, 4)[0]
            out.append((kind, seq, bytes(self.buf[8:8 + n])))
            del self.buf[:12 + n]


class TcpLink:
    def __init__(self, host, port=51021):
        self.s = socket.create_connection((host, port), timeout=5)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def send(self, data):
        self.s.sendall(data)

    def recv(self, timeout):
        self.s.settimeout(max(timeout, 0.001))
        try:
            data = self.s.recv(65536)
        except (socket.timeout, BlockingIOError):
            return b''
        if not data:
            raise BridgeError('connection closed')
        return data

    def close(self):
        self.s.close()


class SerialLink:
    def __init__(self, port, baud=3000000, boot_s=1.5):
        import serial
        self.serial = serial.Serial()
        self.serial.port = port
        self.serial.baudrate = baud
        self.serial.timeout = 0.05
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.open()
        self.baud = baud
        self.boot_s = boot_s

    def enter_bridge(self):
        self.serial.baudrate = 115200
        time.sleep(self.boot_s)
        self.serial.reset_input_buffer()
        self.serial.write(f'\r\nbridge {self.baud}\r\n'.encode())
        time.sleep(0.1)
        self.serial.baudrate = self.baud
        time.sleep(0.05)
        self.serial.reset_input_buffer()

    def send(self, data):
        self.serial.write(data)

    def recv(self, timeout):
        self.serial.timeout = max(timeout, 0.001)
        return self.serial.read(max(1, self.serial.in_waiting or 1))

    def close(self):
        self.serial.close()


class Bridge:
    def __init__(self, link):
        self.link = link
        self.parser = Parser()
        self.seq = 0
        self.data = collections.deque()
        self.ends = collections.deque()
        self.responses = {}
        self.hello_info = None

    @classmethod
    def open(cls, target, baud=3000000):
        if target.startswith('tcp:'):
            host, _, port = target[4:].partition(':')
            b = cls(TcpLink(host, int(port or 51021)))
            b.hello()
            return b
        link = SerialLink(target, baud)
        b = cls(link)
        try:
            b.hello(timeout=0.4)
        except BridgeError:
            link.enter_bridge()
            b.hello()
        return b

    def close(self):
        try:
            self.request(BYE, timeout=1)
        except BridgeError:
            pass
        self.link.close()

    def _pump(self, timeout):
        for kind, seq, payload in self.parser.push(self.link.recv(timeout)):
            if kind == RX_DATA:
                self.data.append(payload)
            elif kind == RX_END:
                self.ends.append(payload)
            elif kind & 0x80 and kind < RX_DATA:
                self.responses[(kind & 0x7F, seq)] = payload

    def request(self, kind, payload=b'', timeout=3.0):
        self.seq = (self.seq + 1) & 0xFFFF
        seq = self.seq
        self.link.send(encode(kind, seq, payload))
        end = time.monotonic() + timeout
        while (kind, seq) not in self.responses:
            left = end - time.monotonic()
            if left <= 0:
                raise BridgeError(f'request 0x{kind:02X} timed out')
            self._pump(min(left, 0.05))
        r = self.responses.pop((kind, seq))
        if len(r) < 4:
            raise BridgeError('short response')
        return struct.unpack_from('<i', r)[0], r[4:]

    def call(self, kind, payload=b'', timeout=3.0):
        status, rest = self.request(kind, payload, timeout)
        if status:
            raise BridgeError(f'request 0x{kind:02X}', status)
        return rest

    def hello(self, timeout=2.0):
        rest = self.call(HELLO, struct.pack('<H', 1), timeout)
        version, caps, rom_major, rom_minor = struct.unpack_from('<HHBB', rest)
        name = rest[16:48].split(b'\0')[0].decode(errors='replace')
        board = rest[48:80].split(b'\0')[0].decode(errors='replace')
        self.hello_info = dict(version=version, caps=caps, rom=(rom_major, rom_minor), name=name, id=board)
        return self.hello_info

    def set_rx(self, frequency_hz, bandwidth_hz=31250, step=13, boost=0, path=2, route=0, code=0, rate_hz=0):
        p = struct.pack('<IIBBB', frequency_hz, bandwidth_hz, step, boost, path)
        if route:
            p += struct.pack('<BBI', route, code, rate_hz)
        rest = self.call(SET_RX, p, timeout=5)
        errors, rate = struct.unpack_from('<HI', rest)
        return errors, rate

    def set_gain(self, step, boost=0):
        self.call(SET_GAIN, bytes([step, boost]), timeout=5)

    def get_gain(self, timeout=2.0):
        status, rest = self.request(GET_GAIN, timeout=timeout)
        if len(rest) != 14:
            raise BridgeError('GET_GAIN', status)
        step, manual, value, rssi, t = struct.unpack('<BBHHQ', rest)
        return dict(status=status, step=step, manual=manual, gain_value=value, rssi_raw=rssi, device_time=t)

    def time_us(self):
        return struct.unpack('<Q', self.call(GET_TIME)[:8])[0]

    def rx_start(self, duration_ms=0, fmt=CS16, capacity=1024):
        self.data.clear()
        self.ends.clear()
        p = struct.pack('<HI', capacity, duration_ms) + (bytes([fmt]) if fmt else b'')
        generation, rate = struct.unpack('<II', self.call(RX_START, p, timeout=5)[:8])
        return generation, rate

    def rx_stop(self, timeout=3.0):
        self.call(RX_STOP)
        return self.wait_end(timeout)

    def wait_end(self, timeout=3.0):
        end = time.monotonic() + timeout
        while not self.ends:
            left = end - time.monotonic()
            if left <= 0:
                raise BridgeError('no RX_END')
            self._pump(min(left, 0.05))
        e = self.ends.popleft()
        names = ('generation', 'capture_error', 'recovery_error', 'reason', 'fifo_flags', 'max_fifo_level',
                 'dropped', 'gain_changes', 'pairs', 'start_time', 'elapsed_us', 'max_service_gap_us')
        return dict(zip(names, struct.unpack('<IiiBBHIIQQQQ', e)))

    def read(self, timeout=0.2):
        end = time.monotonic() + timeout
        while not self.data:
            left = end - time.monotonic()
            if left <= 0 or self.ends:
                return None
            self._pump(min(left, 0.05))
        p = self.data.popleft()
        names = ('generation', 'rate_hz', 'first_pair', 'start_time', 'dropped', 'pairs', 'format', 'shift')
        h = dict(zip(names, struct.unpack_from('<IIQQIHBB', p)))
        return h, p[32:]


def to_iq(h, raw):
    import numpy as np
    f, n = h['format'], h['pairs']
    if f == CS16:
        v = np.frombuffer(raw, '<i2', 2 * n).astype(np.float64)
        return (v[0::2] + 1j * v[1::2]) / 32768.0
    if f == CS12:
        b = np.frombuffer(raw, np.uint8, 3 * n).reshape(n, 3).astype(np.int32)
        w = b[:, 0] | b[:, 1] << 8 | b[:, 2] << 16
        i = ((w & 0xFFF) ^ 0x800) - 0x800
        q = ((w >> 12 & 0xFFF) ^ 0x800) - 0x800
        return (i + 1j * q) * float(1 << h['shift']) / 32768.0
    b = np.frombuffer(raw, np.int8, 2 * n).astype(np.float64)
    if f == IQ6G:
        b = np.floor(b / 4.0) / 32.0
    else:
        b = b / 128.0
    return b[0::2] + 1j * b[1::2]


def iq6_steps(h, raw):
    import numpy as np
    if h['format'] != IQ6G:
        return None
    b = np.frombuffer(raw, np.uint8, 2 * h['pairs'])
    return (b[0::2] & 3) | (b[1::2] & 3) << 2
