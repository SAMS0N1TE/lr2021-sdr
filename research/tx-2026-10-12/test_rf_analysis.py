import json,pathlib,subprocess,sys
import numpy as np
rng=np.random.default_rng(20261012);fs=2000000;rate=9600
state=0x13579bdf;payload=[]
for _ in range(64):
 state^=(state<<13)&0xffffffff;state^=state>>17;state^=(state<<5)&0xffffffff;state&=0xffffffff;payload.append(state&255)
bits=np.unpackbits(np.array(payload,np.uint8)).astype(int)*2-1
# Preamble and sync precede the known payload, matching the baseline diagnostic.
prefix=np.r_[np.tile([1,-1],16),np.unpackbits(np.array([0xd3,0x91,0xda,0x26],np.uint8)).astype(int)*2-1]
symbols=np.r_[prefix,bits];samples=int(np.ceil(len(symbols)*fs/rate))
f=symbols[np.minimum((np.arange(samples)*rate/fs).astype(int),len(symbols)-1)]*10000+250800
wave=0.2*np.exp(2j*np.pi*np.cumsum(f)/fs)
x=(rng.normal(size=fs)+1j*rng.normal(size=fs))*.002
start=fs//4;x[start:start+samples]+=wave
packed=np.column_stack([x.real,x.imag]);packed=np.clip(packed*127,-127,127).astype(np.int8)
root=pathlib.Path('rf');root.mkdir(exist_ok=True);packed.tofile(root/'analyzer-selftest.iq8')
subprocess.run([sys.executable,'analyze_rf.py','analyzer-selftest'],check=True,stdout=subprocess.DEVNULL)
r=json.loads((root/'analyzer-selftest.analysis.json').read_text())
assert r['prefix_matches']['512']['errors']==0,r
assert abs(r['carrier_offset_hz']-800)<500,r
assert abs(r['prefix_matches']['512']['start_s']-(.25+64/rate))<1/rate,r
print('Synthetic RF analyzer validation: 512/512 payload bits, carrier offset and payload timing passed.')
