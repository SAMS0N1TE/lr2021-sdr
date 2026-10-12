import numpy as np,pathlib,json,sys
for name in sys.argv[1:]:
 rate=4800 if '4800' in name else 9600
 z=np.fromfile(pathlib.Path('rf')/(name+'.iq8'),np.int8).reshape(-1,2).astype(float)
 y=np.mean(((z[:,0]+1j*z[:,1])/128).reshape(-1,8)*np.exp(-2j*np.pi*np.arange(8)/8),axis=1);fs=250000
 power=np.convolve(abs(y)**2,np.ones(50)/50,'same');active=np.flatnonzero(power>np.median(power)*30);s,e=active[0]/fs,active[-1]/fs
 f=np.angle(y[1:]*y[:-1].conj())*fs/(2*np.pi);data=np.array([-96,-64,-32,0,32,64,96,0]);best=None
 for start in np.arange(s-.0002,s+.001,1/fs):
  obs=[];ph=[]
  for i in range(8):
   a=int((start+(i*8+2)/rate)*fs);b=int((start+(i*8+6)/rate)*fs)
   obs.append(np.mean(f[a:b]));ph.append(np.mean(y[a:b]))
  r=np.corrcoef(data,obs)[0,1];coef=np.polyfit(data,obs,1)
  if best is None or r>best[0]:best=(r,start,coef,obs)
 r,start,coef,obs=best
 result=dict(name=name,rate=rate,burst=[s,e],clipped_fraction=float(np.mean(abs(z)>=127)),correlation=float(r),start=float(start),hz_per_count=float(coef[0]),offset_hz=float(coef[1]),plateau_hz=obs)
 print(json.dumps(result,indent=2));(pathlib.Path('rf')/(name+'.steps.json')).write_text(json.dumps(result,indent=2))
