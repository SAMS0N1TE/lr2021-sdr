#!/usr/bin/env python3
"""Validate bounded LakeShark LR2021 source-1/source-2 receive captures.

Requires experimental firmware with ``lora lrlab`` and pyserial. NumPy is
required only for the optional spectrum. Raw capture files stay in --out;
review them before sharing. This tool does not transmit or flash firmware.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re

from lr2021_research import Console


def parse_lab(output, source, mode):
    """Reject incomplete serial transfers and reported FIFO/recovery loss."""
    lines = [line.strip() for line in output.replace("\r", "").splitlines()]
    summaries = [line for line in lines if line.startswith("lrlab: f=")]
    if len(summaries) != 1:
        raise ValueError("Expected one lrlab capture summary")
    fields = dict(re.findall(r"(\w+)=([^\s]+)", summaries[0]))
    for key in ("f", "source", "bw", "gain", "bytes", "elapsed_us", "max_level", "max_gap_us"):
        fields[key] = int(fields[key])
    for key in ("flags", "errors"):
        fields[key] = int(fields[key], 16)
    if fields["source"] != source or (mode == "discard" and fields["buf"] != "discard") or \
       (mode != "discard" and fields["buf"] not in ("int", "psram")) or \
       int(fields["post_rx_gain"]) != (0 if mode == "hexpre" else 1):
        raise ValueError("Wrong source or capture mode")
    if fields["result"] != "ESP_OK" or fields["recovery"] != "ESP_OK" or \
       fields["errors"] or fields["flags"] & 0x30 or fields["stopped"] != "0":
        raise ValueError("Firmware reported FIFO, radio or recovery failure")
    width = 2 if source == 1 else 4
    if fields["bytes"] <= 0 or fields["bytes"] % width or fields["elapsed_us"] <= 0:
        raise ValueError("Missing or unaligned sample bytes")
    if mode == "discard":
        if any(line.startswith("lrlab-hex:") for line in lines):
            raise ValueError("Unexpected hex transfer in discard mode")
        data = None
    else:
        data = bytearray()
        for line in lines:
            m = re.fullmatch(r"lrlab-hex: (\d+) ([0-9A-Fa-f]+)", line)
            if m:
                if int(m[1]) != len(data):
                    raise ValueError("Missing, duplicate or reordered hex block")
                data.extend(bytes.fromhex(m[2]))
        if "lrlab-hex: end" not in lines or len(data) != fields["bytes"]:
            raise ValueError("Incomplete serial sample transfer")
        data = bytes(data)
    fields.update(pairs=fields["bytes"] // width,
                  estimated_pair_rate=round(fields["bytes"] * 1000000 /
                                            (width * fields["elapsed_us"])),
                  sha256=hashlib.sha256(data).hexdigest() if data is not None else None,
                  format_hypothesis="interleaved_iq8" if source == 1 else "interleaved_iq16_le",
                  rate_verified=False,
                  continuity="no_reported_FIFO_loss_not_sequence_verified")
    return fields, data


def spectral_peaks(data, source, assumed_rate):
    """List isolated complex spectral peaks; rate and I/Q layout are hypotheses."""
    import numpy as np
    v = np.frombuffer(data, dtype=np.int8 if source == 1 else "<i2")
    z = v[0::2].astype(float) + 1j * v[1::2].astype(float)
    if len(z) < 128 or assumed_rate <= 0:
        raise ValueError("Spectrum requires at least 128 pairs and a positive assumed rate")
    z -= z.mean()
    n = len(z)
    power = 20 * np.log10(np.maximum(np.abs(np.fft.fftshift(np.fft.fft(z * np.hanning(n)))), 1e-9))
    hz = np.fft.fftshift(np.fft.fftfreq(n, 1 / assumed_rate))
    floor = float(np.median(power[np.abs(hz) > 2500]))
    chosen = []
    for i in np.argsort(power)[::-1]:
        if abs(hz[i]) <= 2500 or any(abs(hz[i] - p["hz"]) < 500 for p in chosen):
            continue
        chosen.append(dict(hz=round(float(hz[i])), above_median_db=round(float(power[i] - floor), 2)))
        if len(chosen) == 8:
            break
    return dict(assumed_rate_hz=assumed_rate, peaks=chosen,
                frequency_axis_verified=False, analog_passband_measured=False)


def capture(a):
    if not 104000 <= a.khz <= 2740000 or not 0 <= a.gain <= 13 or \
       not 0 <= a.bw <= 255 or not 1 <= a.ms <= (250 if a.mode == "discard" else 100):
        raise ValueError("Frequency, bandwidth code, gain or duration outside firmware bounds")
    if a.assumed_rate is not None and (a.assumed_rate <= 0 or a.mode == "discard"):
        raise ValueError("Assumed rate requires a positive Hz value and hex capture")
    a.out.mkdir(parents=True, exist_ok=False)
    c = Console(a.port)
    try:
        version = c.command("version")
        radio = c.command("lora")
        (a.out / "identity-private.txt").write_text(version + radio, encoding="utf-8")
        if "LR2021 FW 1.24" not in radio:
            raise ValueError("Expected LR2021 ROM 1.24; no probe sent")
        command = f"lora lrlab {a.khz} fsk {a.source} {a.bw} {a.ms} {a.gain} {a.mode}"
        output = c.command(command, timeout=25)
        (a.out / "console-private.txt").write_text(output, encoding="utf-8")
        fields, data = parse_lab(output, a.source, a.mode)
        if fields["f"] != a.khz or fields["bw"] != a.bw or fields["gain"] != a.gain:
            raise ValueError("Firmware summary differs from request")
        if data is not None:
            (a.out / "iq-private.bin").write_bytes(data)
        if data is not None and a.assumed_rate:
            fields["spectrum"] = spectral_peaks(data, a.source, a.assumed_rate)
        after = c.command("lora")
        (a.out / "after-private.txt").write_text(after, encoding="utf-8")
        if "LR2021 FW 1.24" not in after or "errors=0x0000" not in after:
            raise ValueError("Post-capture radio status is not clean")
        (a.out / "report-private.json").write_text(json.dumps(fields, indent=2), encoding="utf-8")
        print(json.dumps({k: fields[k] for k in
                          ("f", "source", "bw", "bytes", "elapsed_us", "estimated_pair_rate",
                           "max_level", "max_gap_us", "flags")}, indent=2))
        if "spectrum" in fields:
            print(json.dumps(fields["spectrum"], indent=2))
    finally:
        c.close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--port", required=True)
    p.add_argument("--khz", type=int, required=True)
    p.add_argument("--source", type=int, choices=(1, 2), required=True)
    p.add_argument("--bw", type=int, required=True, help="FSK rx_bw byte, validated by firmware")
    p.add_argument("--ms", type=int, required=True)
    p.add_argument("--gain", type=int, default=13)
    p.add_argument("--mode", choices=("hex", "hexpre", "discard"), default="hex")
    p.add_argument("--assumed-rate", type=int, help="Hz for exploratory FFT; not a clock measurement")
    p.add_argument("--out", type=Path, required=True)
    capture(p.parse_args())


if __name__ == "__main__":
    main()
