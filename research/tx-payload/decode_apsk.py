"""Decode 8-APSK using training only for timing, CFO and common complex gain.
Payload bytes are sliced and CRC-checked before any expected-data comparison.
"""
import json,pathlib,sys,binascii,numpy as np
name=sys.argv[1];rate=int(sys.argv[2]);repeats=int(sys.argv[3]);root=pathlib.Path('rf');fs=62500
frame=json.loads(pathlib.Path(sys.argv[4] if len(sys.argv)>4 else 'v9-frame.json').read_text());train=np.array(frame['training_symbols']);nsym=len(frame['symbols']);frame_samples=nsym*8
const=np.array([complex(round((48 if k&4 else 24)*np.cos(np.pi/4+(k&3)*np.pi/2)),round((48 if k&4 else 24)*np.sin(np.pi/4+(k&3)*np.pi/2))) for k in range(8)])
z=np.fromfile(root/(name+'.iq8'),np.int8).reshape(-1,2).astype(float);y=np.mean(((z[:,0]+1j*z[:,1])/128).reshape(-1,32)*np.exp(-2j*np.pi*np.arange(32)/8),axis=1)
pw=np.convolve(abs(y)**2,np.ones(13)/13,'same');noise=float(np.median(pw));active=np.flatnonzero(pw>noise*30);s=active[0]/fs
def samples(start,count):
 # Four received measurements spread within the held portion of each symbol.
 tt=start+(np.arange(count)[:,None]*8+np.array([5.2,5.8,6.4,7.0]))/rate
 pos=tt*fs;idx=np.floor(pos).astype(int);fraction=pos-idx
 if idx.min()<0 or idx.max()+1>=len(y):raise ValueError('Capture does not contain the complete frame')
 v=y[idx]*(1-fraction)+y[idx+1]*fraction
 return tt,v
results=[];origin=None
for number in range(repeats):
 anchor=s if origin is None else origin+number*frame_samples/rate
 starts=np.arange(anchor-.0002,anchor+.0006,4/fs) if origin is None else np.arange(anchor-.5/rate,anchor+.5/rate,1/fs)
 best=None
 def fit(starts,freqs):
  global best
  for st in starts:
   tt,v=samples(st,len(train));tt=tt-st
   vv=np.mean(v[None,:,:]*np.exp(-2j*np.pi*freqs[:,None,None]*tt),axis=2)
   want=const[train];gain=vv@want.conj()/np.vdot(want,want).real
   err=np.sum(abs(vv-gain[:,None]*want)**2,axis=1)/np.sum(abs(gain[:,None]*want)**2,axis=1);k=int(np.argmin(err))
   if best is None or err[k]<best[0]:best=(float(err[k]),float(st),float(freqs[k]),complex(gain[k]))
 fit(starts,np.arange(-600,101,2))
 fit(np.arange(best[1]-4/fs,best[1]+4.1/fs,.25/fs),np.arange(best[2]-3,best[2]+3.01,.25))
 err,st,cfo,gain=best
 if origin is None:origin=st
 tt,v=samples(st,nsym);obs=np.mean(v*np.exp(-2j*np.pi*cfo*(tt-st)),axis=1)/gain
 decisions=np.argmin(abs(obs[:,None]-const[None,:])**2,axis=1)
 bits=''.join(f'{int(k):03b}' for k in decisions[len(train):]);raw=bytes(int(bits[i:i+8],2) for i in range(0,80,8));payload=raw[:8];actual=int.from_bytes(raw[8:10],'big');calc=binascii.crc_hqx(payload,0xffff)
 expected=bytes.fromhex(frame['payload_hex']);ber=sum((a^b).bit_count() for a,b in zip(payload,expected))
 results.append(dict(frame=number,start_s=st,cfo_hz=cfo,training_evm_percent=100*np.sqrt(err),decoded_payload_hex=payload.hex(),decoded_payload_ascii=payload.decode('ascii',errors='replace'),received_crc=f'{actual:04X}',calculated_crc=f'{calc:04X}',crc_ok=actual==calc,payload_bit_errors=ber,payload_bits=64,ring_symbol_errors=int(np.sum((decisions[len(train):]&4)!=(np.array(frame['data_symbols'])&4))),data_symbol_errors=int(np.sum(decisions[len(train):]!=np.array(frame['data_symbols']))),decoded_symbols=decisions.tolist()))
r=dict(name=name,rate=rate,repeats=repeats,clipped_fraction=float(np.mean(abs(z)>=127)),frames=results,all_crc_ok=all(x['crc_ok'] for x in results),total_payload_bits=64*repeats,total_payload_bit_errors=sum(x['payload_bit_errors'] for x in results),calibration='training symbols only; one timing/CFO/complex gain per frame; nearest 8-APSK constellation; payload CRC checked independently')
(root/(name+'.apsk.json')).write_text(json.dumps(r,indent=2));print(json.dumps(r,indent=2))
