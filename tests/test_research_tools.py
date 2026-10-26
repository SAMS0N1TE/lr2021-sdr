"""Evidence-integrity and synthetic receiver tests; no RF hardware claims."""
import sys
from pathlib import Path
import struct
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
import casic_probe as cp
import lr2021_research as lr
import lr2021_lab as lab
try:
    import numpy
except ImportError:
    numpy = None


class CasicTests(unittest.TestCase):
    def test_vendor_empty_port_poll_vector(self):
        self.assertEqual(cp.packet(6,0).hex(),"bace0000060000000600")

    def test_documented_configuration_decode(self):
        port=cp.packet(6,0,bytes.fromhex("00ffc00800c20100"))
        msg=cp.packet(6,1,bytes.fromhex("4e000100"))
        rate=cp.packet(6,4,struct.pack("<I",1000))
        ack=cp.packet(5,1,bytes.fromhex("06010000"))
        frames=cp.inspect(port+msg+rate+ack)["frames"]
        self.assertEqual(frames[0]["port"]["baud"],115200)
        self.assertTrue(frames[0]["port"]["binary_output"])
        self.assertEqual(frames[1]["message_rate"],dict(cls=0x4e,id=0,every_n_fixes=1))
        self.assertEqual(frames[2]["position_interval_ms"],1000)
        self.assertEqual(frames[3]["ack_for"],dict(cls=6,id=1,accepted=True))

    def test_one_shot_measurement_query_vector(self):
        self.assertEqual(cp.named_query("measurements_once").hex(),
                         "bace040006010310ffff07100501")
        self.assertEqual(cp.named_query("measurements_stream").hex(),
                         "bace040006010310010007100701")
        self.assertEqual(cp.named_query("measurements2_stream").hex(),
                         "bace040006010311010007110701")

    def test_mixed_nmea_and_binary(self):
        ver=b"TEST_SW\0".ljust(32,b"\0")+b"TEST_HW\0".ljust(32,b"\0")
        data=b"$GNTXT,unrelated*00\r\n"+cp.packet(10,4,ver)
        result=cp.inspect(data)
        self.assertEqual(result["frames"][0]["software"],"TEST_SW")
        self.assertFalse(result["raw_iq_verified"])

    def test_corrupt_and_truncated_resynchronize(self):
        broken=bytearray(cp.packet(6,4,struct.pack("<I",1000)))
        broken[-1]^=1
        result=cp.inspect(bytes(broken)+cp.packet(10,4)+b"\xba\xce\x00")
        self.assertEqual(len(result["frames"]),1)
        self.assertEqual([x["issue"] for x in result["issues"]],["bad_checksum","truncated_header"])

    def test_invalid_length_does_not_swallow_next_frame(self):
        result=cp.inspect(b"\xba\xce\xff\xff\x01\x02"+cp.packet(6,0))
        self.assertEqual(result["frames"][0]["cls"],6)
        self.assertEqual(result["issues"][0]["issue"],"invalid_length")

    def test_bridge_transfer_integrity(self):
        import zlib
        raw=b"$GNX\r\n"+cp.packet(6,4,struct.pack("<I",1000))
        summary=f"gps-probe: begin name=rate bytes={len(raw)} dropped=0 tx=10 tx_error=ESP_OK complete=1\n"
        transfer=summary+f"gps-probe-hex: 000000 {raw.hex().upper()}\n"+f"gps-probe: end bytes={len(raw)} crc32={zlib.crc32(raw):08X}\n"
        self.assertEqual(cp.parse_bridge_output(transfer,"rate"),raw)
        for damaged in (transfer.replace("dropped=0","dropped=1"),
                        transfer.replace("000000","000004"),
                        transfer.replace("crc32=","crc32=F")):
            with self.assertRaises(ValueError):cp.parse_bridge_output(damaged,"rate")
        stream=transfer.replace("name=rate", "name=measurements_stream").replace(
            "tx=10", "tx=14 cleanup_tx=14 cleanup_error=ESP_OK")
        self.assertEqual(cp.parse_bridge_output(stream,"measurements_stream"),raw)
        self.assertEqual(cp.parse_bridge_output(stream.replace("name=measurements_stream",
                                                        "name=measurements2_stream"),
                                                "measurements2_stream"),raw)
        with self.assertRaises(ValueError):
            cp.parse_bridge_output(stream.replace("cleanup_tx=14","cleanup_tx=0"),
                                   "measurements_stream")


SUMMARY="lrprobe: f=1575420 kHz path=HF bw=31250 rate=125000 gain=13 boost=4 tune=ESP_OK errors=0x0000 bytes=4 buf=int max_level=44 polls=1 elapsed_us=40 flags=0x00 result=ESP_OK recovery=ESP_OK\n"
HEX="lrprobe-hex: 000000 01000200\nlrprobe-hex: end\n"


class CaptureTests(unittest.TestCase):
    def test_complete_iq(self):
        meta,data=lr.parse_capture(SUMMARY+HEX)
        self.assertEqual(data,b"\x01\x00\x02\x00")
        self.assertEqual(meta["pairs"],1)
        self.assertFalse(meta["rate_verified"])

    def test_missing_reordered_and_fifo_loss_rejected(self):
        for text in (SUMMARY+HEX.replace("000000","000004"), SUMMARY+HEX.replace("lrprobe-hex: end", ""),
                     SUMMARY.replace("flags=0x00","flags=0x10")+HEX,
                     SUMMARY.replace("recovery=ESP_OK","recovery=ESP_FAIL")+HEX):
            with self.assertRaises(ValueError):lr.parse_capture(text)

    def test_lab_capture_integrity_and_discard(self):
        summary=("lrlab: f=2320010 modem=fsk source=1 bw=163 gain=13 post_rx_gain=1 "
                 "bytes=4 elapsed_us=10 max_level=20 max_gap_us=5 flags=0x00 "
                 "errors=0x0000 stopped=0 buf=int result=ESP_OK recovery=ESP_OK\n")
        transfer=summary+"lrlab-hex: 000000 0102FEFD\nlrlab-hex: end\n"
        fields,data=lab.parse_lab(transfer,1,"hex")
        self.assertEqual(data,b"\x01\x02\xfe\xfd")
        self.assertEqual(fields["pairs"],2)
        self.assertFalse(fields["rate_verified"])
        for damaged in (transfer.replace("000000","000002"),
                        transfer.replace("flags=0x00","flags=0x10"),
                        transfer.replace("lrlab-hex: end", "")):
            with self.assertRaises(ValueError):lab.parse_lab(damaged,1,"hex")
        discard=summary.replace("buf=int","buf=discard")
        self.assertIsNone(lab.parse_lab(discard,1,"discard")[1])
        pre=transfer.replace("post_rx_gain=1","post_rx_gain=0")
        self.assertEqual(lab.parse_lab(pre,1,"hexpre")[1],data)
        with self.assertRaises(ValueError):lab.parse_lab(pre,1,"hex")


@unittest.skipIf(numpy is None, "NumPy is needed for GNSS tools")
class GnssTests(unittest.TestCase):
    def test_lab_spectrum_known_complex_tone(self):
        import numpy as np
        rate=200000
        t=np.arange(2000)/rate
        tone=20*np.exp(-2j*np.pi*20000*t)
        iq=np.empty(4000,dtype=np.int8)
        iq[0::2]=np.rint(tone.real).astype(np.int8)
        iq[1::2]=np.rint(tone.imag).astype(np.int8)
        result=lab.spectral_peaks(iq.tobytes(),1,rate)
        self.assertLess(abs(result["peaks"][0]["hz"]+20000),200)
        self.assertFalse(result["analog_passband_measured"])

    def test_capacity_and_energy_bounds(self):
        rows=lr.budget()["rows"]
        self.assertTrue(all(0<r["ca_power_fraction"]<1 for r in rows))
        self.assertEqual(sorted(r["ca_power_fraction"] for r in rows),[r["ca_power_fraction"] for r in rows])
        self.assertLess(rows[2]["iq8_spi_payload_fraction"],1)
        self.assertGreater(rows[2]["iq16_spi_payload_fraction"],1)

    def test_synthetic_code_and_doppler_recovered(self):
        import numpy as np
        import gnss_acquire as ga
        # Known PRN 7, code phase 19 samples, +1500 Hz, seeded noise.
        rate=250000
        code=ga.reference(7,rate,125000,1500)
        signal=np.tile(np.roll(code,19),20)
        t=np.arange(signal.size)/rate
        rng=np.random.default_rng(20261026)
        x=signal*np.exp(2j*np.pi*1500*t)+0.7*(rng.normal(size=t.size)+1j*rng.normal(size=t.size))
        result=ga.acquire(x,rate,125000,[3,7,10],2000,500,20)
        self.assertEqual(result["candidates"][0]["prn"],7)
        self.assertEqual(result["candidates"][0]["offset_hz"],1500)
        self.assertEqual(result["candidates"][0]["code_lag_sample"],19)
        self.assertIsNone(result["detected"])

    def test_prn1_icd_prefix(self):
        import gnss_acquire as ga
        # IS-GPS-200 first ten PRN-1 chips: octal 1440, binary 1100100000.
        self.assertEqual(''.join('1' if x<0 else '0' for x in ga.ca(1)[:10]),'1100100000')


if __name__=="__main__":unittest.main()
