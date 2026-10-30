#!/usr/bin/env python3
# usage: sc.py [-p port] [-o outfile] [-t timeout] "cmd1" "cmd2" ...  ; waits for a line containing 'done' / 'st=' / '->' per cmd
import serial, sys, time, argparse
ap=argparse.ArgumentParser(); ap.add_argument('-p',default='/dev/ttyACM0'); ap.add_argument('-o'); ap.add_argument('-t',type=float,default=10); ap.add_argument('cmds',nargs='*')
a=ap.parse_args()
s=serial.Serial(a.p,115200,timeout=0.2)
time.sleep(0.3); s.read(100000)
out=open(a.o,'w') if a.o else None
for c in a.cmds:
    s.write((c+'\r').encode()); end=time.time()+a.t; buf=b''
    while time.time()<end:
        d=s.read(65536)
        if d:
            buf+=d; end=time.time()+a.t
            last=buf.rstrip().split(b'\n')[-1]
            if last.endswith(b'done') or last.split(b' ')[0] in (b'rc',b'mw',b'err',b'hold') or b'->' in last: break
    txt=buf.decode(errors='replace').replace('\r','')
    if out: out.write(txt)
    else: print(txt)
