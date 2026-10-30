# For each undocumented opcode: param length check, registers/RAM touched (handler + 1 level of callees)
import re,sys
L=open('disasm/rom_annotated.txt').read().split('\n')
idx={}
for i,l in enumerate(L):
    m=re.match(r'\s*([0-9a-f]+):',l)
    if m: idx[int(m.group(1),16)]=i
def body(a,maxn=150):
    i=idx[a]; out=[]
    for n in range(maxn):
        s=L[i+n]; out.append(s)
        if n>2 and (re.search(r'leave_s.*pcl',s) or re.search(r'\tj_s\s+\[blink\]',s)): 
            # keep going if more code from same func follows a branch target; simple heuristic: stop
            break
    return out
def consts(lines):
    c=set()
    for s in lines:
        for x in re.findall(r'0x(f[0-9a-f]{5}|80[0-9a-f]{4})\b',s): c.add(x)
    return c
def calls(lines):
    t=set()
    for s in lines:
        m=re.search(r'; -> ([0-9a-f]+)',s) or re.search(r'\tbl(?:\.d)?\s+\d+\s+;([0-9a-f]+)',s)
        if m: t.add(int(m.group(1),16))
    return t
for l in open('analysis/opcode_table.txt'):
    p=l.split()
    if len(p)<3 or p[2] not in sys.argv[1:]: continue
    a=int(p[1],16); b=body(a); c=consts(b); cl=calls(b)
    sub=set()
    for t in cl:
        if t in idx and t not in (0x23c0,0x23b0):
            bb=body(t,120); sub|=consts(bb)
    lens=re.findall(r'(?:cmp_s|brlo[.a-z]*|brhs[.a-z]*|bc\.d|blo_s)\s+r0,(0x[0-9a-f]+|\d+)',' '.join(b[:8]))
    print(f'{p[0]} @{a:#x} minlen?{lens[:1]} regs:{sorted(c)} callees:{[hex(x) for x in sorted(cl)]} sub-regs:{sorted(sub-c)}')
