# SPDX-License-Identifier: MIT
"""Runs the lr2021_tcp program on the in-process simulator and checks it the
way an rtl_tcp client sees it: the 12-byte header, commands, then the u8 I/Q
stream with the simulated tone where it should be.
  python3 tests/test_rtl_tcp_client.py build/lr2021_tcp"""
import cmath,math,socket,struct,subprocess,sys,threading,time

TONE=433930000

def main(exe):
    p=subprocess.Popen([exe,'-d',f'sim=tone,tone={TONE}','-p','0','-f','433.9M','-s','125k'],
                       stderr=subprocess.PIPE,text=True)
    log=[];ready=threading.Event()
    def drain():
        for line in p.stderr:
            log.append(line.rstrip());sys.stderr.write(line)
            if 'listening on' in line:ready.set()
    threading.Thread(target=drain,daemon=True).start()
    try:
        assert ready.wait(10),'server did not start'
        port=int(next(l for l in log if 'listening on' in l).rsplit(':',1)[1])
        s=socket.create_connection(('127.0.0.1',port),timeout=5)
        head=b''
        while len(head)<12:head+=s.recv(12-len(head))
        assert head[:4]==b'RTL0' and struct.unpack('>II',head[4:])==(5,29),head
        def cmd(c,v):s.sendall(struct.pack('>BI',c,v&0xffffffff))
        cmd(0x01,TONE-10000);cmd(0x02,2048000);cmd(0x03,1);cmd(0x0d,14);cmd(0x05,0);cmd(0x08,0)
        time.sleep(0.5)
        s.setblocking(False)
        try:
            while s.recv(1<<20):pass
        except BlockingIOError:pass
        s.setblocking(True)
        data=bytearray();t0=time.time()
        while time.time()-t0<1.0:data+=s.recv(1<<16)
        rate=len(data)/2/(time.time()-t0)
        z=[complex(data[i]-127.5,data[i+1]-127.5) for i in range(0,40000,2)]
        turn=sum(cmath.phase(z[i+1]/z[i]) for i in range(len(z)-1) if z[i] and z[i+1])
        offset=turn/(len(z)-1)*250000/(2*math.pi)
        amplitude=sum(abs(v) for v in z)/len(z)
        print(f'{rate:.0f} pairs/s, tone at {offset:+.0f} Hz, amplitude {amplitude:.1f}')
        assert 200000<rate<300000,rate
        assert abs(offset-10000)<200,offset
        assert 40<amplitude<110,amplitude     # level 11 of the 0x0d index 14
        s.close()
        assert any('set the client to 250000' in l for l in log)
        print('lr2021_tcp answered as an rtl_tcp server')
    finally:
        p.terminate();p.wait(10)

if __name__=='__main__':
    main(sys.argv[1])
