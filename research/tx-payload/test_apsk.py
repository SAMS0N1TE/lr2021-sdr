import pathlib,json,numpy as np,subprocess,sys
f=json.loads(pathlib.Path('v9-frame.json').read_text());q=np.array(f['iq_pairs']);q=q[:,0]+1j*q[:,1];fs=2000000;rate=9600
rng=np.random.default_rng(3);n=160000;t=np.arange(n)/fs;pos=(t-.02)*rate;on=(pos>=0)&(pos<len(q)-1)
for corrupt in [False,True]:
 qq=q.copy()
 if corrupt:qq[(16+3)*8:(16+4)*8]*=-1
 v=np.zeros(n,complex);v[on]=.001*(np.interp(pos[on],np.arange(len(q)),qq.real)+1j*np.interp(pos[on],np.arange(len(q)),qq.imag))*np.exp(.6j)
 v*=np.exp(2j*np.pi*(250000-271)*t);v+=.004*(rng.standard_normal(n)+1j*rng.standard_normal(n))
 name='_apsk_corrupt' if corrupt else '_apsk_ok';np.clip(np.round(np.column_stack([v.real,v.imag])*128),-128,127).astype(np.int8).tofile(pathlib.Path('rf')/(name+'.iq8'))
 subprocess.run([sys.executable,'decode_apsk.py',name,str(rate),'1'],check=True,capture_output=True)
 r=json.loads((pathlib.Path('rf')/(name+'.apsk.json')).read_text());assert r['all_crc_ok']!=corrupt,r
 print(name,'CRC',r['all_crc_ok'],'bit errors',r['total_payload_bit_errors'])
