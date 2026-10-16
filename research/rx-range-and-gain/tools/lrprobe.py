"""Drive `lora lrprobe` on the LR2021 board and log results.

usage: python lrprobe.py <out.csv> <probe-args-file | -> --serial <USB serial of the board>
Each line of the args file is the part after `lora lrprobe`, e.g. `80010 31250 0 -1 auto 40`.
Lines starting with '#' are skipped. Prints one compact line per probe.
"""
import csv, re, sys, time
import serial
from serial.tools import list_ports

out_path, args_path = sys.argv[1], sys.argv[2]
if "--serial" not in sys.argv:
    sys.exit("--serial <USB serial> is required")
sn = sys.argv[sys.argv.index("--serial") + 1]
lines = sys.stdin.read().splitlines() if args_path == "-" else open(args_path).read().splitlines()
probes = [l.strip() for l in lines if l.strip() and not l.strip().startswith("#")]

port = next((p.device for p in list_ports.comports() if p.serial_number == sn), None)
if not port:
    sys.exit("board %s not found" % sn)
s = serial.Serial(); s.port = port; s.baudrate = 115200; s.timeout = 0.1
s.dtr = False; s.rts = False
s.open()
time.sleep(0.3); s.read(65536)

KV = re.compile(r"(\w+)=(\S+)")
PK = re.compile(r"pk(\d)=([+-]\d+)Hz:([+-][\d.]+)")
fields = ["args", "f_khz", "path", "bw", "rate", "gain", "boost", "tune", "errors", "bytes", "buf",
          "max_level", "elapsed_us", "flags", "result", "recovery", "dc", "rms_dbfs", "floor",
          "pk1_hz", "pk1_db", "pk2_hz", "pk2_db", "pk3_hz", "pk3_db", "err_names"]
with open(out_path, "a", newline="") as f:
    w = csv.DictWriter(f, fieldnames=fields)
    if f.tell() == 0:
        w.writeheader()
    for a in probes:
        s.read(65536)
        s.write(b"lora lrprobe " + a.encode() + b"\r\n")
        buf, end, done = "", time.time() + 8.0, False
        while time.time() < end and not done:
            buf += s.read(8192).decode("utf-8", "replace")
            if "lrprobe-spec:" in buf and buf.rstrip().endswith(("dB", ")")) or "usage: lora lrprobe" in buf:
                done = True
            # the spec line ends with a newline; wait for it
            m = re.search(r"lrprobe-spec:[^\n]*\n", buf)
            if m or ("lrprobe:" in buf and "bytes=0 " in buf and re.search(r"lrprobe:[^\n]*\n", buf)):
                done = True
        time.sleep(0.05); buf += s.read(8192).decode("utf-8", "replace")
        row = {"args": a}
        m = re.search(r"lrprobe:([^\n]*)", buf)
        if m:
            line = m.group(1)
            for k, v in KV.findall(line):
                if k == "f": k = "f_khz"
                if k in fields: row[k] = v
            n = re.search(r"errors=0x[0-9A-F]{4} ([A-Z_|]+) bytes", line)
            row["err_names"] = n.group(1) if n else ""
        m = re.search(r"lrprobe-spec:([^\n]*)", buf)
        if m:
            line = m.group(1)
            d = dict(KV.findall(line))
            row["dc"] = d.get("dc", "")
            r = re.search(r"\(([-\d.]+) dBFS\)", line); row["rms_dbfs"] = r.group(1) if r else ""
            fl = re.search(r"floor=([-\d.]+)", line); row["floor"] = fl.group(1) if fl else ""
            for i, hz, db in PK.findall(line):
                row["pk%s_hz" % i], row["pk%s_db" % i] = hz, db
        if "usage:" in buf and "lrprobe" in buf and "lrprobe:" not in buf:
            row["result"] = "USAGE"
        if not m and "lrprobe:" not in buf:
            row["result"] = row.get("result") or "NO_REPLY:" + buf.strip().replace("\r", " ").replace("\n", " | ")[-160:]
        w.writerow(row); f.flush()
        print("%-34s %s %s err=%s %s rms=%s pk1=%s:%s pk2=%s:%s %s" % (
            a, row.get("path", "?"), row.get("tune", "?"), row.get("errors", "?"), row.get("err_names", ""),
            row.get("rms_dbfs", "?"), row.get("pk1_hz", "?"), row.get("pk1_db", "?"),
            row.get("pk2_hz", "?"), row.get("pk2_db", "?"), row.get("result", "?")), flush=True)
s.close()
