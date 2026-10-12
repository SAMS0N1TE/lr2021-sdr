import numpy as np,pathlib,json,sys
name=sys.argv[1] if len(sys.argv)>1 else 'v2-raw0-steps';z=np.fromfile(pathlib.Path('rf')/(name+'.iq8'),np.int8).reshape(-1,2).astype(float)
y=np.mean(((z[:,0]+1j*z[:,1])/128).reshape(-1,8)*np.exp(-2j*np.pi*np.arange(8)/8),axis=1);fs=250000;rate=9600
power=np.convolve(abs(y)**2,np.ones(50)/50,'same');active=np.flatnonzero(power>np.median(power)*30);start=active[0]/fs+.0001
phase=np.unwrap(np.angle(y));times=np.arange(len(y))/fs
slopes=[];amps=[]
for i in range(8):
 a=int((start+(i*8+2)/rate)*fs);b=int((start+(i*8+6)/rate)*fs)
 slopes.append(np.polyfit(times[a:b],phase[a:b],1)[0]/(2*np.pi));amps.append(float(np.mean(abs(y[a:b]))))
cfo=np.median(slopes);ph=[]
for i in range(8):
 a=int((start+(i*8+2)/rate)*fs);b=int((start+(i*8+6)/rate)*fs)
 ph.append(float(np.angle(np.mean(y[a:b]*np.exp(-2j*np.pi*cfo*times[a:b])))))
result=dict(cfo=float(cfo),slopes=[float(v) for v in slopes],amps=amps,phase_rad=ph,relative_phase_deg=(np.angle(np.exp(1j*(np.array(ph)-ph[0])))*180/np.pi).tolist())
print(json.dumps(result,indent=2));(pathlib.Path('rf')/(name+'.phase.json')).write_text(json.dumps(result,indent=2))
