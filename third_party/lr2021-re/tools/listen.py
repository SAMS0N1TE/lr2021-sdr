# usage: [PORT=/dev/ttyACMx] listen.py <secs> [cmds...]   prints RXSTAT lines
import serial, sys, time, os
s=serial.Serial(os.environ.get('PORT','/dev/ttyACM0'),115200,timeout=0.2); time.sleep(0.3)
for c in sys.argv[2:]: s.write((c+'\r').encode()); time.sleep(0.5)
end=time.time()+float(sys.argv[1]); buf=b''
while time.time()<end:
    buf+=s.read(4096)
    while b'\n' in buf:
        l,buf=buf.split(b'\n',1); l=l.decode(errors='replace').strip()
        if 'RXSTAT' in l or 'rc st' in l: print(time.strftime('%H:%M:%S'), l, flush=True)
    if int(time.time())%5==0: s.write(b'\r'); time.sleep(1)
