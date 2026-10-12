import sys,json,pathlib,numpy as np
name=sys.argv[1];rate=19200 if '19200' in name else 4800 if '4800' in name else 9600
z=np.fromfile(pathlib.Path('rf')/(name+'.iq8'),np.int8).reshape(-1,2).astype(float)
y=np.mean(((z[:,0]+1j*z[:,1])/128).reshape(-1,8)*np.exp(-2j*np.pi*np.arange(8)/8),axis=1);fs=250000
pw=np.convolve(abs(y)**2,np.ones(50)/50,'same');active=np.flatnonzero(pw>np.median(pw)*30);s=active[0]/fs
x=0x13579bdf;p=[]
for _ in range(32):
 x^=(x<<13)&0xffffffff;x^=x>>17;x^=(x<<5)&0xffffffff;x&=0xffffffff;p.append(x&255)
q=np.array(p,np.uint8).view(np.int8).reshape(-1,2).astype(float);phi=np.angle(q[:,0]+1j*q[:,1]);results={}
for position in range(4):
 best=None
 for start in np.arange(s-.0001,s+.00025,1/fs):
  tt=start+(np.arange(16)*4+position+.5)/rate;obs=y[(tt*fs).astype(int)];obs=obs/abs(obs)*np.exp(-1j*phi)
  cfo=np.arange(-600,101,2);corr=np.mean(obs[None,:]*np.exp(-2j*np.pi*cfo[:,None]*(tt-tt[0])),axis=1);j=np.argmax(abs(corr))
  if best is None or abs(corr[j])>best['coherence']:
   residual=np.angle(obs*np.exp(-2j*np.pi*cfo[j]*(tt-tt[0]))/corr[j])
   best=dict(coherence=float(abs(corr[j])),start_s=float(start),cfo_hz=int(cfo[j]),phase_rms_deg=float(np.sqrt(np.mean(residual**2))*180/np.pi),residual_deg=(residual*180/np.pi).tolist(),iq=q.tolist())
 results[str(position)]=best
r=dict(name=name,rate=rate,hold_samples=4,clipped_fraction=float(np.mean(abs(z)>=127)),fits_by_sample_position=results)
print(json.dumps(r,indent=2));(pathlib.Path('rf')/(name+'.hold.json')).write_text(json.dumps(r,indent=2))
