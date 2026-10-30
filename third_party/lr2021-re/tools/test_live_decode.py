import unittest
import time
from unittest.mock import patch

from live_decode import one_capture


class SerialCapture:
    def __init__(self, chunks):
        self.timeout = .2
        self.chunks = []
        self.dump_chunks = chunks
        self.commands = []

    def reset_input_buffer(self):
        self.chunks.clear()

    def write(self, data):
        self.commands.append(data)
        if data == b'streamdump\r':
            self.chunks.extend(self.dump_chunks)
        else:
            self.chunks.append(b'stream done\r\n')

    def read(self, n):
        if not self.chunks:
            return b''
        chunk = self.chunks.pop(0)
        if len(chunk) > n:
            self.chunks.insert(0, chunk[n:])
        return chunk[:n]


class CaptureTransferTests(unittest.TestCase):
    def test_usb_keepalive_is_ignored_newline(self):
        class DelayedSerial(SerialCapture):
            def write(self, data):
                self.commands.append(data)
                if data == b'\n':
                    self.chunks.append(b'stream done\n')
                elif data == b'streamdump\r':
                    self.chunks.extend(self.dump_chunks)

            def read(self, n):
                if not self.chunks:
                    time.sleep(.01)
                return super().read(n)

        s = DelayedSerial([b'hit=1\nSTREAMBIN 2\n\x00\x01'])
        _, data = one_capture(s, 'stream 0 3 8000 8 8 3', 8)
        self.assertEqual(data, b'\x00\x01')
        self.assertEqual(s.commands, [b'stream 0 3 8000 8 8 3\r', b'\n', b'streamdump\r'])

    def test_early_completion_and_fragmented_header(self):
        # Binary bytes can contain newlines and the firmware's text markers.
        iq = b'\r\nSTREAMBIN\x00\xff\x01'
        s = SerialCapture([b'stream src=0 hit=1\r\nSTREAMBIN',
                           f' {len(iq)}\r\n'.encode(), iq[:3], iq[3:]])
        with patch('live_decode.time.sleep') as sleep:
            summary, data = one_capture(s, 'stream 0 3 8000 8 8 3', 8)
        sleep.assert_not_called()
        self.assertEqual(summary, 'stream src=0 hit=1')
        self.assertEqual(data, iq)
        self.assertEqual(s.commands[-1], b'streamdump\r')
        self.assertEqual(s.timeout, .2)

    def test_truncated_transfer(self):
        s = SerialCapture([b'hit=1\nSTREAMBIN 100\n\x00\x01'])
        with self.assertRaisesRegex(ValueError, 'truncated IQ transfer'):
            one_capture(s, 'stream 0 3 8000 8 8 3', 8)
        self.assertEqual(s.timeout, .2)

    def test_odd_byte_count(self):
        s = SerialCapture([b'hit=1\nSTREAMBIN 3\nabc'])
        with self.assertRaisesRegex(ValueError, 'odd IQ byte count'):
            one_capture(s, 'stream 0 3 8000 8 8 3', 8)


if __name__ == '__main__':
    unittest.main()
