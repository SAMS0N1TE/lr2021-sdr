#!/usr/bin/env python3
"""Bounded CASIC inspection for a direct GNSS UART or LakeShark research bridge.

Direct listen only records bytes unless --poll is supplied; bridge requires a
named query through experimental LakeShark firmware. Requires pyserial live.
No reset/save/baud/protocol-mode writes.
Raw data may contain location; keep captures private until reviewed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import time
import zlib

# Quectel L76K V1.1: CFG-PRT/CFG-RATE. Broader chipset protocol/gpsd:
# MON-VER, MON-HW, NAV-CLOCK, RXM-MEASX. Availability is unverified on this L76K.
QUERIES = {"port": (0x06, 0x00), "rate": (0x06, 0x04),
           "version": (0x0A, 0x04), "hardware": (0x0A, 0x09),
           "clock": (0x01, 0x11), "measurements": (0x03, 0x10),
           "gpsinfo": (0x01, 0x20), "bdsinfo": (0x01, 0x21),
           "glninfo": (0x01, 0x22), "position": (0x01, 0x03)}


def checksum(header_payload):
    if len(header_payload) % 4:
        raise ValueError("CASIC checksum requires aligned words")
    return sum(x[0] for x in struct.iter_unpack("<I", header_payload)) & 0xFFFFFFFF


def packet(cls, ident, payload=b""):
    if not 0 <= cls < 256 or not 0 <= ident < 256 or len(payload) > 4096 or len(payload) % 4:
        raise ValueError("Invalid CASIC frame")
    body = struct.pack("<HBB", len(payload), cls, ident) + payload
    return b"\xba\xce" + body + struct.pack("<I", checksum(body))


def inspect(data):
    frames, bad = [], []
    i = 0
    while i < len(data):
        if data[i:i+2] != b"\xba\xce":
            i += 1
            continue
        if len(data) - i < 10:
            bad.append(dict(offset=i, issue="truncated_header"))
            break
        size, cls, ident = struct.unpack_from("<HBB", data, i + 2)
        if size > 4096 or size % 4:
            bad.append(dict(offset=i, issue="invalid_length", payload_bytes=size))
            i += 1
            continue
        end = i + 10 + size
        if end > len(data):
            bad.append(dict(offset=i, issue="truncated_frame", payload_bytes=size))
            i += 1
            continue
        expected = struct.unpack_from("<I", data, end - 4)[0]
        if checksum(data[i+2:end-4]) != expected:
            bad.append(dict(offset=i, issue="bad_checksum"))
            i += 1
            continue
        payload = data[i+6:end-4]
        record = dict(offset=i, cls=cls, id=ident, payload_bytes=size, payload_hex=payload.hex())
        if (cls, ident) == QUERIES["version"] and size >= 64:
            record["software"] = payload[:32].split(b"\0", 1)[0].decode("ascii", errors="replace")
            record["hardware"] = payload[32:64].split(b"\0", 1)[0].decode("ascii", errors="replace")
        frames.append(record)
        i = end
    return dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest(), frames=frames,
                issues=bad, raw_observables_verified=False, raw_iq_verified=False)


def parse_bridge_output(output, name):
    """Validate a LakeShark gps-probe console transfer before trusting bytes."""
    starts = re.findall(r"^gps-probe: begin ([^\r\n]+)$", output, re.M)
    ends = re.findall(r"^gps-probe: end bytes=(\d+) crc32=([0-9A-Fa-f]{8})$", output, re.M)
    if len(starts) != 1 or len(ends) != 1:
        raise ValueError("Expected one complete gps-probe transfer")
    fields = dict(re.findall(r"(\w+)=([^ ]+)", starts[0]))
    if fields.get("name") != name or fields.get("complete") != "1" or fields.get("tx_error") != "ESP_OK":
        raise ValueError("Probe name, completion or UART write failed")
    if int(fields.get("dropped", "-1")) != 0:
        raise ValueError("Binary capture overflowed")
    data = bytearray()
    for off, hexdump in re.findall(r"^gps-probe-hex: (\d+) ([0-9A-Fa-f]+)$", output, re.M):
        if int(off) != len(data):
            raise ValueError("Missing or reordered hex block")
        data.extend(bytes.fromhex(hexdump))
    if len(data) != int(fields["bytes"]) or len(data) != int(ends[0][0]):
        raise ValueError("Transferred byte count differs from firmware")
    if zlib.crc32(data) != int(ends[0][1], 16):
        raise ValueError("Transfer CRC differs from firmware")
    expected_tx = 0 if name == "passive" else 10
    if int(fields.get("tx", "-1")) != expected_tx:
        raise ValueError("Incomplete query write")
    return bytes(data)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="action", required=True)
    q = sub.add_parser("query")
    q.add_argument("name", choices=QUERIES)
    f = sub.add_parser("inspect")
    f.add_argument("file", type=Path)
    l = sub.add_parser("listen")
    l.add_argument("--port", required=True, help="Direct GNSS UART only")
    l.add_argument("--baud", type=int, choices=(9600, 19200, 38400, 57600, 115200), default=115200)
    l.add_argument("--seconds", type=int, choices=range(1, 31), default=5)
    l.add_argument("--out", type=Path, required=True)
    l.add_argument("--poll", choices=QUERIES)
    b = sub.add_parser("bridge", help="LakeShark research firmware gps probe over its console")
    b.add_argument("--port", required=True, help="LakeShark console, not direct GNSS UART")
    b.add_argument("--poll", choices=("passive", *QUERIES), required=True)
    b.add_argument("--ms", type=int, choices=range(100, 2001), default=700)
    b.add_argument("--out", type=Path, required=True)
    a = p.parse_args()
    if a.action == "query":
        print(packet(*QUERIES[a.name]).hex(" "))
    elif a.action == "inspect":
        print(json.dumps(inspect(a.file.read_bytes()), indent=2))
    elif a.action == "bridge":
        from lr2021_research import Console
        a.out.mkdir(parents=True, exist_ok=False)
        c = Console(a.port)
        try:
            output = c.command(f"gps probe {a.poll} {a.ms}", timeout=15)
        finally:
            c.close()
        (a.out / "console-private.txt").write_text(output, encoding="utf-8")
        data = parse_bridge_output(output, a.poll)
        (a.out / "uart-private.bin").write_bytes(data)
        report = inspect(data)
        report.update(poll=a.poll, window_ms=a.ms, source="LakeShark binary bridge")
        (a.out / "report-private.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(json.dumps(dict(bytes=len(data), issues=len(report["issues"]),
                              frames=[dict(cls=f["cls"], id=f["id"], payload_bytes=f["payload_bytes"])
                                      for f in report["frames"]]), indent=2))
    else:
        import serial
        a.out.mkdir(parents=True, exist_ok=False)
        s = serial.Serial(port=None, baudrate=a.baud, timeout=0.1, write_timeout=2)
        s.port, s.dtr, s.rts = a.port, False, False
        data = bytearray()
        try:
            s.open()
            if a.poll:
                s.write(packet(*QUERIES[a.poll]))
            end = time.monotonic() + a.seconds
            while time.monotonic() < end and len(data) < 400000:
                data.extend(s.read(min(max(1, s.in_waiting), 4096)))
        finally:
            s.close()
            (a.out / "uart.bin").write_bytes(data)
            report = inspect(data)
            report.update(poll=a.poll, baud=a.baud, elapsed_limit_seconds=a.seconds)
            (a.out / "report.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(json.dumps({k: report[k] for k in ("bytes", "issues")}, indent=2))


if __name__ == "__main__":
    main()
