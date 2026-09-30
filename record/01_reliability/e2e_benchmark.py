#!/usr/bin/env python3
"""Serial NEW/CANCEL latency with exact response validation, one warmup + five runs."""
import hashlib,json,os,signal,socket,statistics,struct,subprocess,sys,tempfile,time
from pathlib import Path
root=Path(__file__).resolve().parents[2];phase=sys.argv[1];binary=root/'build/reliability-release/exchange_main'
request=struct.Struct('!2sBBQQBIIQbqI');response=struct.Struct('!2sBBQQBIIQQbqIIBQq')
assert request.size==50 and response.size==79
def receive(peer):
    data=b''
    while len(data)<response.size:
        part=peer.recv(response.size-len(data))
        if not part:raise RuntimeError('unexpected EOF')
        data+=part
    return response.unpack(data)
def percentile(values,p):return sorted(values)[min(len(values)-1,int((len(values)-1)*p))]
cpus=sorted(os.sched_getaffinity(0))[:4];samples=[];directories=[]
for run in range(6):
    # Fail if the endpoint is occupied; never stop an unrelated user process.
    probe=socket.socket();occupied=probe.connect_ex(('127.0.0.1',12345))==0;probe.close()
    if occupied:raise RuntimeError('12345 is occupied; existing process preserved')
    directory=Path(tempfile.mkdtemp(prefix='trade-e2e-perf-'));directories.append(str(directory))
    env=os.environ.copy();env['TRADE_SNAPSHOT_MS']='60000'
    output=open(directory/'process_output.log','w')
    process=subprocess.Popen(['taskset','-c',','.join(map(str,cpus)),str(binary)],cwd=directory,env=env,stdout=output,stderr=subprocess.STDOUT)
    try:
        deadline=time.monotonic()+20;peer=None
        while time.monotonic()<deadline:
            if process.poll() is not None:raise RuntimeError((process.returncode,(directory/'process_output.log').read_text()))
            candidate=socket.socket();candidate.settimeout(.2)
            try:candidate.connect(('127.0.0.1',12345));peer=candidate;break
            except OSError:candidate.close();time.sleep(.05)
        if peer is None:raise RuntimeError('startup deadline')
        peer.settimeout(10);peer.setsockopt(socket.IPPROTO_TCP,socket.TCP_NODELAY,1)
        epoch=0x770000+run;latencies=[];checksum=0
        for i in range(200):
            for kind,wanted in [(1,1),(2,2)]:
                sequence=len(latencies)+1;payload=request.pack(b'TS',1,1,sequence,epoch,kind,96,0,i+1,1,100,1)
                begin=time.perf_counter_ns();peer.sendall(payload);result=receive(peer);latencies.append(time.perf_counter_ns()-begin)
                if result[:3]!=(b'TS',1,2) or result[3]!=sequence or result[4]!=epoch or result[5]!=wanted or result[6]!=96 or result[8]!=i+1:raise RuntimeError(('response differs',result))
                checksum+=result[8]
        sample={'operations':len(latencies),'checksum':checksum,'p50_ns':percentile(latencies,.5),'p99_ns':percentile(latencies,.99),'mean_ns':statistics.mean(latencies),'total_latency_ns':sum(latencies)}
        if run:samples.append(sample)
        print(phase,run,sample,flush=True)
        # Keep the TCP connection available during orderly server drainage.
        process.send_signal(signal.SIGTERM)
        try:sample['stop_exit_code']=process.wait(timeout=10)
        except subprocess.TimeoutExpired:process.kill();process.wait();sample['stop_exit_code']=process.returncode
        if phase!='before' and sample['stop_exit_code']!=0:raise RuntimeError(('orderly stop failed',sample,directory))
        peer.close()
    finally:
        if process.poll() is None:process.kill();process.wait()
        output.close()
data={'commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),'dirty':bool(subprocess.check_output(['git','status','--porcelain'],cwd=root,text=True)),'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'cpu_affinity':cpus,'warmups':1,'repetitions':5,'new_orders_per_run':200,'cancel_orders_per_run':200,'input':'client96,ticker0,order1..200,BUY,price100,qty1; serial ACKs','samples':samples,'median_p50_ns':statistics.median(s['p50_ns'] for s in samples),'median_p99_ns':statistics.median(s['p99_ns'] for s in samples),'evidence_directories':directories}
(root/'record/01_reliability'/('perf_e2e_'+phase+'.json')).write_text(json.dumps(data,indent=2)+'\n')
print('MEDIAN',phase,data['median_p50_ns'],data['median_p99_ns'],flush=True)
