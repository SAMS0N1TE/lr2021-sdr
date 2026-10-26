#!/usr/bin/env python3
"""Receive-only LakeShark probes and idealized GNSS bandwidth calculations.

Capture needs pyserial; budget needs NumPy. No flash, reset, TX or radio
configuration writes beyond the firmware's bounded lrprobe session.
"""
import argparse
import datetime
import hashlib
import json
import math
from pathlib import Path
import re
import time


PLANS = {
    "gnss": [(1575020, 31250), (1575420, 31250), (1575820, 31250)],
    "wifi": [(2412000, 62500), (2437000, 62500), (2462000, 62500)],
}


def budget():
    import numpy as np
    rows = []
    for rate in (125000, 250000, 664800, 1000000, 2046000):
        # Ideal rectangular chip PSD, normalized integral over all frequencies=1.
        # B is a hypothetical two-sided complex passband, NOT measured LR BW.
        f = np.linspace(-rate / 2, rate / 2, 200001)
        y = np.sinc(f / 1023000) ** 2
        fraction = float(np.sum((y[1:]+y[:-1])*np.diff(f)*0.5) / 1023000)
        rows.append(dict(ideal_passband_hz=rate, ca_power_fraction=fraction,
                         ideal_matched_filter_loss_db=-10 * math.log10(fraction),
                         iq8_spi_payload_fraction=rate * 2 / 2000000,
                         iq16_spi_payload_fraction=rate * 4 / 2000000))
    return dict(kind="calculation_not_RF_measurement", ca_chip_rate_hz=1023000,
                spi_hz=16000000, command_overhead_included=False,
                analog_filter_measured=False, rows=rows)


def parse_capture(text):
    lines = [line.strip() for line in text.replace("\r", "").splitlines()]
    summaries = [line for line in lines if line.startswith("lrprobe: f=")]
    if len(summaries) != 1:
        raise ValueError("Expected exactly one capture summary")
    fields = dict(re.findall(r"(\w+)=([^\s]+)", summaries[0]))
    for key in ("f", "bw", "rate", "gain", "boost", "bytes", "max_level", "polls", "elapsed_us"):
        fields[key] = int(fields[key])
    for key in ("errors", "flags"):
        fields[key] = int(fields[key], 16)
    if any(fields.get(k) != "ESP_OK" for k in ("tune", "result", "recovery")):
        raise ValueError("Firmware reported tuning/capture/recovery failure")
    if fields["flags"] & 0x30 or fields["errors"]:
        raise ValueError("Radio errors or FIFO overflow/underflow invalidate capture")
    if not fields["bytes"] or fields["bytes"] % 4:
        raise ValueError("Missing or unaligned IQ16 samples")
    data = bytearray()
    for line in lines:
        m = re.fullmatch(r"lrprobe-hex: (\d+) ([0-9A-Fa-f]+)", line)
        if m:
            if int(m[1]) != len(data):
                raise ValueError("Missing, duplicate or reordered hex block")
            data.extend(bytes.fromhex(m[2]))
    if "lrprobe-hex: end" not in lines or len(data) != fields["bytes"]:
        raise ValueError("Incomplete serial sample transfer")
    fields.update(pairs=len(data)//4, sha256=hashlib.sha256(data).hexdigest(),
                  rate_verified=False, continuity="no_reported_FIFO_loss_not_sequence_verified",
                  signal_identification="unknown")
    return fields, bytes(data)


class Console:
    def __init__(self, port):
        import serial
        self.s = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=2)
        self.s.port = port
        self.s.dtr = False
        self.s.rts = False
        self.s.open()
        try:
            self.s.set_buffer_size(rx_size=1 << 20)
        except (AttributeError, NotImplementedError):
            pass

    def command(self, command, timeout=15):
        self.s.write(command.encode("ascii") + b"\r\n")
        data = bytearray()
        deadline = time.monotonic() + timeout
        # Require the echoed command before accepting a prompt left by old output.
        while time.monotonic() < deadline:
            data.extend(self.s.read(max(1, self.s.in_waiting)))
            if command.encode() in data and b"lakeshark>" in data.split(command.encode(), 1)[1]:
                return data.decode(errors="replace").replace("\r", "")
        raise TimeoutError("Command did not complete: " + command + "\n" + data[-1000:].decode(errors="replace"))

    def close(self):
        self.s.close()


def capture(args):
    # A new directory prevents accidentally overwriting prior evidence.
    args.out.mkdir(parents=True, exist_ok=False)
    records = []
    complete = False
    c = Console(args.port)
    try:
        version = c.command("version")
        identity = c.command("lora")
        (args.out / "identity.txt").write_text(version + identity, encoding="utf-8")
        if "LR2021" not in identity or "FW 1.24" not in identity:
            raise ValueError("Expected LR2021 ROM 1.24; no probe sent")
        for repeat in range(args.repeats):
            for khz, bw in PLANS[args.plan]:
                cmd = f"lora lrprobe {khz} {bw} 13 -1 hf 40 hex"
                stem = f"{args.plan}-{repeat}-{khz}"
                output = c.command(cmd)
                (args.out / (stem + ".txt")).write_text(output, encoding="utf-8")
                try:
                    fields, data = parse_capture(output)
                except ValueError as exc:
                    records.append(dict(command=cmd, log=stem + ".txt", accepted=False, error=str(exc)))
                    raise
                if fields["f"] != khz or fields["bw"] != bw or fields["rate"] != bw * 4:
                    raise ValueError("Unexpected tuning/rate in response")
                (args.out / (stem + ".ci16")).write_bytes(data)
                records.append(dict(command=cmd, file=stem + ".ci16", accepted=True, **fields))
                print(json.dumps({k: fields[k] for k in ("f", "rate", "bytes", "max_level", "flags")}), flush=True)
        (args.out / "after.txt").write_text(c.command("lora"), encoding="utf-8")
        complete = True
    finally:
        c.close()
        (args.out / "manifest.json").write_text(json.dumps(dict(
            schema=1, complete=complete, date_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
            antenna_note=args.antenna, nominal_format="ci16_le", captures=records), indent=2), encoding="utf-8")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="action", required=True)
    b = sub.add_parser("budget")
    b.add_argument("--out", type=Path)
    c = sub.add_parser("capture")
    c.add_argument("--port", required=True)
    c.add_argument("--plan", choices=PLANS, required=True)
    c.add_argument("--out", type=Path, required=True)
    c.add_argument("--antenna", required=True, help="Antenna/feed/input and environment; local metadata")
    c.add_argument("--repeats", type=int, choices=range(1, 6), default=1)
    a = p.parse_args()
    if a.action == "budget":
        result = json.dumps(budget(), indent=2)
        if a.out:
            a.out.write_text(result + "\n", encoding="utf-8")
        print(result)
    else:
        capture(a)


if __name__ == "__main__":
    main()
