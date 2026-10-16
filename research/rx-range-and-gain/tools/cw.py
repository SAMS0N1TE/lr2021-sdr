"""HackRF CW carrier on an ISM frequency for a fixed time; exits by itself (-n), do not kill it mid-stream.

usage: python cw.py <MHz> <seconds> [txvga 0..47] [dac 1..127]
Refuses anything outside 433.05 to 434.79, 902 to 928 and 2400 to 2483.5 MHz (ISM only).
Amp stays off.
"""
import subprocess, sys

mhz, secs = float(sys.argv[1]), float(sys.argv[2])
vga = int(sys.argv[3]) if len(sys.argv) > 3 else 0
dac = int(sys.argv[4]) if len(sys.argv) > 4 else 127
ism = [(433.05, 434.79), (902.0, 928.0), (2400.0, 2483.5)]
if not any(lo <= mhz <= hi for lo, hi in ism):
    sys.exit("refused: %.4f MHz is not ISM" % mhz)
if not (0 <= vga <= 47 and 1 <= dac <= 127 and 0 < secs <= 900):
    sys.exit("bad args")
rate = 2000000
cmd = ["hackrf_transfer", "-f", str(int(mhz * 1e6)), "-s", str(rate),
       "-a", "0", "-x", str(vga), "-c", str(dac), "-n", str(int(rate * secs))]
print(" ".join(cmd), flush=True)
sys.exit(subprocess.call(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
