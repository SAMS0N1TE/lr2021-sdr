import sys,pathlib,json,numpy as np
name=sys.argv[1];meta=json.loads((pathlib.Path('rf')/(name+'.hold.json')).read_text());rate=meta['rate'];start=meta['fits_by_sample_position']['1']['start_s'];q=np.array(meta['fits_by_sample_position']['1']['iq']);want=np.angle(q[:,0]+1j*q[:,1])
z=np.fromfile(pathlib.Path('rf')/(name+'.iq8'),np.int8).reshape(-1,2).astype(float);y=np.mean(((z[:,0]+1j*z[:,1])/128).reshape(-1,8)*np.exp(-2j*np.pi*np.arange(8)/8),axis=1);fs=250000;phase=np.unwrap(np.angle(y));t=np.arange(len(y))/fs
windows=[(int((start+(i*4+2)/rate)*fs),int((start+(i*4+3.5)/rate)*fs)) for i in range(16)]
slopes=[np.polyfit(t[a:b],phase[a:b],1)[0]/(2*np.pi) for a,b in windows];cfo=np.median(slopes)
angles=np.array([np.angle(np.mean(y[a:b]*np.exp(-2j*np.pi*cfo*t[a:b]))) for a,b in windows]);delta=np.angle(np.exp(1j*np.diff(angles)))*180/np.pi;expected=np.angle(np.exp(1j*np.diff(want)))*180/np.pi
result=dict(name=name,cfo_hz=float(cfo),expected_jump_deg=expected.tolist(),measured_jump_deg=delta.tolist(),jump_error_deg=(np.angle(np.exp(1j*(delta-expected)*np.pi/180))*180/np.pi).tolist())
print(json.dumps(result,indent=2));(pathlib.Path('rf')/(name+'.jumps.json')).write_text(json.dumps(result,indent=2))
