import sys, struct
# hex dump lines "AAAAAA: w0 w1 ..." -> binary (words little-endian, as the ARC core sees them)
mem={}
for l in open(sys.argv[1]):
    if ':' not in l or '!st' in l: continue
    a,ws=l.split(':',1)
    try: a=int(a,16)
    except: continue
    for i,w in enumerate(ws.split()): mem[a+4*i]=int(w,16)
base=int(sys.argv[3],16) if len(sys.argv)>3 else 0
end=max(mem)+4
open(sys.argv[2],'wb').write(b''.join(struct.pack('<I',mem.get(a,0)) for a in range(base,end,4)))
nz=[a for a in mem if mem[a]]; print('last nonzero word at %06X'%max(nz))
