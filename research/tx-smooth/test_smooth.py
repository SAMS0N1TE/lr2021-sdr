import numpy as np,json,pathlib,subprocess,sys
q=np.array(json.loads(pathlib.Path('v8-patterns.json').read_text())['12']['iq_pairs']);q=q[:,0]+1j*q[:,1]
fs=2000000;rate=9600;n=120000;t=np.arange(n)/fs;pos=(t-.02)*rate;on=(pos>=0)&(pos<63);rng=np.random.default_rng(34)
for flat in [False,True]:
 y=np.zeros(n,complex);v=np.interp(pos[on],np.arange(64),q.real)+1j*np.interp(pos[on],np.arange(64),q.imag)
 if flat:v=v/abs(v)*32
 y[on]=.001*v*np.exp(.4j);y*=np.exp(2j*np.pi*(250000-270)*t);y+=.004*(rng.standard_normal(n)+1j*rng.standard_normal(n))
 name='_smooth_flat' if flat else '_smooth_iq';np.clip(np.round(np.column_stack([y.real,y.imag])*128),-128,127).astype(np.int8).tofile(pathlib.Path('rf')/(name+'.iq8'))
 subprocess.run([sys.executable,'analyze_smooth.py',name,str(rate),'12'],check=True,capture_output=True)
 r=json.loads((pathlib.Path('rf')/(name+'.smooth.json')).read_text());evm=r['interior_waveform_evm_percent'];assert evm>25 if flat else evm<10,(name,evm)
 print(name,round(evm,3),'percent interior waveform EVM, CFO',r['cfo_hz'])
