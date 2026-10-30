import re, sys
lines=open(sys.argv[1]).read().split('\n')
jli={}
for l in lines:
    m=re.match(r'\s*([0-9a-f]+):\s.*\bb\s+\d+\s+;([0-9a-f]+)',l)
    if m:
        a=int(m.group(1),16)
        if 0x100<=a<0x830 and (a-0x100)%4==0: jli[(a-0x100)//4]=int(m.group(2),16)
out=[]
for l in lines:
    l=re.sub(r'<_binary_rom_bin_end\+0x[0-9a-f]+>','',l)
    m=re.search(r'jli_s\s+0x([0-9a-f]+)',l)
    if m:
        t=jli.get(int(m.group(1),16))
        if t is not None: l+='\t; -> %x'%t
    out.append(l)
open(sys.argv[2],'w').write('\n'.join(out))
print(len(jli),'jli entries')
