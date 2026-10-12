import numpy as np,pathlib,json,sys
name=sys.argv[1] if len(sys.argv)>1 else 'v2-raw0-random';z=np.fromfile(pathlib.Path('rf')/(name+'.iq8'),np.int8).reshape(-1,2).astype(float)
y=np.mean(((z[:,0]+1j*z[:,1])/128).reshape(-1,8)*np.exp(-2j*np.pi*np.arange(8)/8),axis=1);fs=250000;rate=4800 if '4800' in name else 9600
pw=np.convolve(abs(y)**2,np.ones(50)/50,'same');active=np.flatnonzero(pw>np.median(pw)*30);s=active[0]/fs
x=0x13579bdf;p=[]
for _ in range(128):
 x^=(x<<13)&0xffffffff;x^=x>>17;x^=(x<<5)&0xffffffff;x&=0xffffffff;p.append(x&255)
p=np.array(p,np.uint8);q=p.view(np.int8).reshape(-1,2).astype(float)
models={'iq8':np.angle(q[:,0]+1j*q[:,1]),'iq8_conjugate':-np.angle(q[:,0]+1j*q[:,1]),'phase16le':p.view('<i2')*2*np.pi/65536,'phase16be':p.view('>i2')*2*np.pi/65536}
results={}
for key,phi in models.items():
 best=None
 for start in np.arange(s-.0001,s+.001,1/fs):
  tt=start+(np.arange(64)+.5)/rate;obs=y[(tt*fs).astype(int)];obs=obs/abs(obs)*np.exp(-1j*phi)
  cfo=np.arange(-600,101,2);corr=np.mean(obs[None,:]*np.exp(-2j*np.pi*cfo[:,None]*(tt-tt[0])),axis=1);j=np.argmax(abs(corr))
  if best is None or abs(corr[j])>best['coherence']:
   residual=np.angle(obs*np.exp(-2j*np.pi*cfo[j]*(tt-tt[0]))/corr[j])
   best=dict(coherence=float(abs(corr[j])),start_s=float(start),cfo_hz=int(cfo[j]),phase_rms_deg=float(np.sqrt(np.mean(residual**2))*180/np.pi))
 results[key]=best
print(json.dumps(results,indent=2));(pathlib.Path('rf')/(name+'.models.json')).write_text(json.dumps(results,indent=2))
