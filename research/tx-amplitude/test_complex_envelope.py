"""Synthetic check: complex gain/CFO recovery and detection of lost amplitude."""
import json,pathlib,subprocess,sys,numpy as np
q=np.array(json.loads(pathlib.Path('v7-patterns.json').read_text())['10']['plateau_iq']);q=q[:,0]+1j*q[:,1]
fs=2000000;rate=9600;n=100000;t=np.arange(n)/fs;idx=np.floor((t-.02)*rate).astype(int);active=(idx>=0)&(idx<64)
rng=np.random.default_rng(73)
for limited in (False,True):
 signal=np.zeros(n,complex);v=q[idx[active]//8]
 if limited:v=v/abs(v)*40
 signal[active]=.001*v*np.exp(1j*.7)
 signal*=np.exp(2j*np.pi*(250000-270)*t)
 signal+=.004*(rng.standard_normal(n)+1j*rng.standard_normal(n))
 b=np.clip(np.round(np.column_stack([signal.real,signal.imag])*128),-128,127).astype(np.int8)
 name='_synthetic_flat' if limited else '_synthetic_iq';b.tofile(pathlib.Path('rf')/(name+'.iq8'))
 subprocess.run([sys.executable,'analyze_complex_envelope.py',name,str(rate),'10'],check=True,capture_output=True)
 r=json.loads((pathlib.Path('rf')/(name+'.complex-envelope.json')).read_text())
 assert r['settled_plateau_evm_percent'] > 30 if limited else r['settled_plateau_evm_percent'] < 4,r
 print(name,round(r['settled_plateau_evm_percent'],3),'percent plateau EVM')
