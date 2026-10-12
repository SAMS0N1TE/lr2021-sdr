import numpy as np,json,pathlib,sys
x=0x13579bdf;p=[]
for _ in range(64):
 x^=(x<<13)&0xffffffff;x^=x>>17;x^=(x<<5)&0xffffffff;x&=0xffffffff;p.append(x&255)
p=np.array(p,dtype=np.uint8)
for name in sys.argv[1:] or ['probe-raw0','probe-raw1','probe-raw2']:
 rate=4800 if '4800' in name else 9600
 pp=p^255 if 'inverted' in name else p
 z=np.fromfile(pathlib.Path('rf')/(name+'.iq8'),np.int8).reshape(-1,2).astype(float);v=(z[:,0]+1j*z[:,1])/128
 y=np.mean(v.reshape(-1,8)*np.exp(-2j*np.pi*np.arange(8)/8),axis=1);fs=250000
 a=json.loads((pathlib.Path('rf')/(name+'.analysis.json')).read_text());s,e=a['bursts_s'][0]
 f=np.angle(y[1:]*y[:-1].conj())*fs/(2*np.pi);f=np.convolve(f,np.ones(7)/7,'same')
 candidates={'s8':pp.view(np.int8).astype(float),'u8':pp.astype(float),'s16le':pp.view('<i2').astype(float),'s16be':pp.view('>i2').astype(float)}
 print(name)
 for key,data in candidates.items():
  best=(0,None)
  for start in np.arange(s,e-len(data)/rate,1/fs):
   idx=(start*fs+(np.arange(len(data))+.5)*fs/rate).astype(int)
   obs=f[idx];r=np.corrcoef(data,obs)[0,1]
   if abs(r)>best[0]:best=(abs(r),(start,r,np.polyfit(data,obs,1).tolist()))
  print(key,best)
