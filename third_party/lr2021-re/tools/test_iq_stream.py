import struct
import unittest
import zlib

from iq_stream import (HEADER, STATS, START, DATA, END, DURATION, QUEUE_FULL,
                       FrameParser, StreamValidator, StreamError, IQStream)


def wire(kind, seq, first=0, payload=b'', count=0, flags=0, generation=42, version=1):
    header = HEADER.pack(b'LRIQ', version, kind, HEADER.size, generation, seq, first, count,
                         250000, 3, flags, len(payload), zlib.crc32(payload), 0)
    return header[:-4] + struct.pack('<I', zlib.crc32(header[:-4])) + payload


def stats(raw=6, produced=2, sent=2, overflow=0, fifo=4, elapsed=24):
    return STATS.pack(raw, produced, sent, elapsed, 128, fifo, 300, 1024, overflow, 0, 16000000, 0)


class StreamTests(unittest.TestCase):
    def test_fragmented_checked_session(self):
        parser, validator = FrameParser(), StreamValidator()
        blob = (b'iqstream 1000 16 3\r\n' + wire(START, 0, payload=stats(0, 0, 0))
                + wire(DATA, 1, payload=b'\x80\xff\x7f\x00', count=2)
                + wire(END, 2, first=2, payload=stats(), flags=DURATION))
        for byte in blob:
            for frame in parser.feed(bytes([byte])):
                validator.accept(frame)
        self.assertTrue(validator.ended)
        self.assertEqual(validator.samples, 2)

    def test_native_iq16_lengths_and_format_changes(self):
        payload = struct.pack('<hhhh', -32768, 32767, -1234, 5678)
        blob = (wire(START, 0, payload=stats(0, 0, 0), version=2)
                + wire(DATA, 1, payload=payload, count=2, version=2)
                + wire(END, 2, first=2, payload=stats(), flags=DURATION, version=2))
        validator = StreamValidator()
        for frame in FrameParser().feed(blob):
            validator.accept(frame)
        self.assertTrue(validator.ended)
        self.assertEqual(validator.pair_bytes, 4)
        for data in (wire(DATA, 1, payload=b'abcd', count=2, version=2),
                     wire(DATA, 1, payload=b'abcd', count=2)):
            validator = StreamValidator()
            start = FrameParser().feed(wire(START, 0, payload=stats(0, 0, 0), version=2))[0]
            validator.accept(start)
            with self.assertRaises(StreamError):
                validator.accept(FrameParser().feed(data)[0])

    def test_header_corruption_cannot_change_payload_length(self):
        blob = bytearray(wire(DATA, 1, payload=b'ab', count=1))
        blob[36] ^= 1
        with self.assertRaisesRegex(StreamError, 'header checksum'):
            FrameParser().feed(blob)

    def test_payload_corruption(self):
        blob = bytearray(wire(DATA, 1, payload=b'ab', count=1))
        blob[-1] ^= 1
        with self.assertRaisesRegex(StreamError, 'payload checksum'):
            FrameParser().feed(blob)

    def test_magic_in_samples_is_not_a_frame_boundary(self):
        frames = FrameParser().feed(wire(DATA, 1, payload=b'LRIQLRIQ', count=4))
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0].payload, b'LRIQLRIQ')

    def test_sequence_and_sample_gaps_are_rejected(self):
        for second in (wire(DATA, 2, payload=b'ab', count=1),
                       wire(DATA, 1, first=1, payload=b'ab', count=1),
                       wire(DATA, 1, payload=b'ab', count=1, generation=43)):
            validator = StreamValidator()
            frames = FrameParser().feed(wire(START, 0, payload=stats()) + second)
            validator.accept(frames[0])
            with self.assertRaises(StreamError):
                validator.accept(frames[1])

    def test_device_overflow_is_an_explicit_failure(self):
        frames = FrameParser().feed(wire(START, 0, payload=stats(0, 0, 0))
                                   + wire(END, 1, payload=stats(0, 0, 0, overflow=1), flags=QUEUE_FULL))
        validator = StreamValidator()
        validator.accept(frames[0])
        with self.assertRaisesRegex(StreamError, 'error flags'):
            validator.accept(frames[1])
        self.assertEqual(validator.final_stats['queue_overflows'], 1)

    def test_terminal_counts_must_agree(self):
        for bad in (stats(raw=7, produced=2, sent=1), stats(raw=9), stats(produced=3)):
            frames = FrameParser().feed(wire(START, 0, payload=stats(0, 0, 0))
                                       + wire(DATA, 1, payload=b'abcd', count=2)
                                       + wire(END, 2, first=2, payload=bad, flags=DURATION))
            validator = StreamValidator()
            for frame in frames[:-1]:
                validator.accept(frame)
            with self.assertRaises(StreamError):
                validator.accept(frames[-1])

    def test_old_firmware_cannot_silently_record_iq8_as_iq16(self):
        class Port:
            def read(self, count):
                return wire(START, 0, payload=stats(0, 0, 0))
        client = IQStream(Port())
        client.sample_format = 'iq16'
        client.raw_rate, client.requested_decimation = 250000, 3
        with self.assertRaisesRegex(StreamError, 'different format'):
            client.poll()

    def test_reserved_fifo_bits_and_implausible_rate_are_rejected(self):
        for footer in (stats(fifo=0x4e), stats(raw=6, elapsed=100000)):
            validator = StreamValidator()
            frames = FrameParser().feed(wire(START, 0, payload=stats(0, 0, 0))
                                        + wire(DATA, 1, payload=b'abcd', count=2)
                                        + wire(END, 2, first=2, payload=footer, flags=DURATION))
            for frame in frames[:-1]:
                validator.accept(frame)
            with self.assertRaises(StreamError):
                validator.accept(frames[-1])

    def test_truncated_frame_is_not_published(self):
        parser = FrameParser()
        blob = wire(DATA, 1, payload=b'abcd', count=2)
        self.assertEqual(parser.feed(blob[:-1]), [])
        self.assertEqual(parser.feed(blob[-1:])[0].sample_count, 2)


if __name__ == '__main__':
    unittest.main()
