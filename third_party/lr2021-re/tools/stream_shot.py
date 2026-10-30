# usage: stream_shot.py <out.bin> "<stream cmd>" [--rak]   (XIAO on ttyACM0, RAK repeater on ttyACM1)
# Runs the capture, then fetches it with 'streamdump' (long blocking runs lose their own serial output).
import serial, sys, time
out, cmd = sys.argv[1], sys.argv[2]
ms = int(cmd.split()[3])
x = serial.Serial('/dev/ttyACM0', 115200, timeout=0.2); time.sleep(0.3); x.read(1 << 20)
x.write(b'hold on\r'); time.sleep(0.3); x.read(1 << 20)
x.write(cmd.encode() + b'\r')
if '--rak' in sys.argv:
    time.sleep(1.0)
    rak = serial.Serial('/dev/ttyACM1', 115200, timeout=0.3); rak.read(1 << 20); rak.write(b'advert\r')
time.sleep(ms / 1000 + 1.5); x.read(1 << 20)
x.write(b'streamdump\r')
buf = b''; end = time.time() + 20
while time.time() < end and b'STREAMBIN' not in buf: buf += x.read(4096)
if b'STREAMBIN' not in buf: print('NO STREAMBIN, got:', buf[-300:]); sys.exit(1)
i = buf.index(b'STREAMBIN'); nl = buf.index(b'\n', i)
print(buf[:i].decode(errors='replace').strip().split('\n')[-1])
n = int(buf[i:nl].split()[1]); data = buf[nl + 1:]
x.timeout = 2
while len(data) < n:
    d = x.read(n - len(data))
    if not d: break
    data += d
open(out, 'wb').write(data[:n]); print('saved', min(len(data), n), 'of', n, 'bytes ->', out)
