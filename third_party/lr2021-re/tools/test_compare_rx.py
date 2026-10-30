from pathlib import Path
from collections import deque
import time
import unittest

from compare_rx import airtime, compare_packets, decode_capture
from kiss_rx import KissParser, KissReceiver, encode_frame


class KissTests(unittest.TestCase):
    def test_keepalive_is_control_only_and_does_not_transmit(self):
        class FakeSerial:
            def __init__(self):
                self.writes = []
                self.replies = deque()

            def write(self, data):
                self.writes.append(data)
                self.replies.append(encode_frame(6, b'\x97'))

            def read(self, n):
                time.sleep(.002)
                return self.replies.popleft() if self.replies else b''

        port = FakeSerial()
        receiver = KissReceiver(port, lambda p: None)
        receiver.start()
        try:
            self.assertEqual(receiver.request(0x17, timeout=.2), b'')
            time.sleep(.025)
        finally:
            receiver.close()
        self.assertGreaterEqual(len(port.writes), 2)
        self.assertTrue(all(frame == encode_frame(6, b'\x17') for frame in port.writes))

    def test_fragmented_escaped_frames_and_startup_noise(self):
        parser = KissParser()
        message = b'\x01\xc0\xdb\x00'
        stream = b'boot text\r\n' + encode_frame(0, message) + encode_frame(6, b'\xf9\x08\xb0')
        frames = []
        for byte in stream:
            frames += parser.feed(bytes([byte]))
        self.assertEqual(frames, [b'\x00' + message, b'\x06\xf9\x08\xb0'])

    def test_malformed_escape_recovers_at_next_boundary(self):
        parser = KissParser()
        self.assertEqual(parser.feed(b'\xc0\x00\xdb\x01garbage\xc0' + encode_frame(0, b'ok')),
                         [b'\x00ok'])

    def test_modem_metadata_attaches_to_correct_packet(self):
        received = []
        receiver = KissReceiver(None, received.append)
        receiver._frame(b'\x00first', 1)
        receiver._frame(b'\x06\xf9\xfc\xa6', 1.01)
        receiver._frame(b'\x00second', 2)
        receiver._flush_packet()
        self.assertEqual(received[0]['snr_db'], -1)
        self.assertEqual(received[0]['rssi_dbm'], -90)
        self.assertEqual(received[1]['payload'], b'second'.hex())
        self.assertNotIn('rssi_dbm', received[1])


class ComparisonTests(unittest.TestCase):
    def test_airtime_matches_saved_adverts(self):
        self.assertAlmostEqual(airtime(130), .479744)

    def test_duplicates_are_matched_once_and_capture_gaps_are_excluded(self):
        payload = '11' * 130
        rak = [dict(t=t, payload=payload) for t in (1, 2, 3, 4)]
        lr = [dict(t_end=1.01, payload=payload), dict(t_end=3.01, payload=payload),
              dict(t_end=5, payload='22' * 130)]
        captures = [dict(retained_start=0, retained_end=3.5)]
        result = compare_packets(rak, lr, captures)
        self.assertEqual(result['matched'], 2)
        self.assertEqual(result['rak_only_retained_iq'], 1)
        self.assertEqual(result['rak_only_outside_retained_iq'], 1)
        self.assertEqual(result['lr_only'], 1)
        self.assertAlmostEqual(result['lr_miss_fraction_relative_to_rak'], 1 / 3)

    def test_full_packet_not_just_rx_end_must_be_in_iq(self):
        payload = '11' * 130
        result = compare_packets([dict(t=1, payload=payload)], [],
                                 [dict(retained_start=.9, retained_end=1.5)])
        self.assertIsNone(result['lr_miss_fraction_relative_to_rak'])
        self.assertEqual(result['rak_only_outside_retained_iq'], 1)

    def test_timing_guard_excludes_boundary_ambiguity(self):
        payload = '11' * 130
        result = compare_packets([dict(t=1, payload=payload)],
                                 [dict(t_end=1.01, payload=payload)],
                                 [dict(retained_start=0, retained_end=1.01)])
        self.assertEqual(result['matched'], 1)
        self.assertEqual(result['matched_near_iq_boundary'], 1)
        self.assertIsNone(result['lr_miss_fraction_relative_to_rak'])

    def test_identical_payload_received_twice_is_not_deduplicated(self):
        payload = 'abcd'
        result = compare_packets([dict(t=1, payload=payload), dict(t=2, payload=payload)],
                                 [dict(t_end=2.01, payload=payload)],
                                 [dict(retained_start=0, retained_end=3)])
        self.assertEqual(result['matched'], 1)
        self.assertEqual(result['rak_only_retained_iq'], 1)

    def test_adjacent_identical_transmissions_are_not_false_matches(self):
        # A short packet can finish again ~130 ms later. Opposite receivers
        # hearing different copies must not appear to have received both.
        payload = '11' * 12
        result = compare_packets([dict(t=1, payload=payload)],
                                 [dict(t_end=1.14, payload=payload)],
                                 [dict(retained_start=0, retained_end=2)])
        self.assertEqual(result['matched'], 0)
        self.assertEqual(result['lr_only'], 1)
        self.assertEqual(result['rak_only_retained_iq'], 1)

    def test_multiple_packets_in_one_iq_buffer(self):
        root = Path(__file__).resolve().parents[1]
        data = b''.join((root / 'captures/iter' / name).read_bytes() for name in ('c00.bin', 'c01.bin'))
        attempts = decode_capture(data, 250000 / 3, 7, 62500)
        packets = [a for a in attempts if a.get('crc_ok')]
        self.assertEqual(len(packets), 2)
        self.assertNotEqual(packets[0]['payload'], packets[1]['payload'])
        self.assertLess(packets[0]['packet_end_s'], packets[1]['packet_start_s'])


if __name__ == '__main__':
    unittest.main()
