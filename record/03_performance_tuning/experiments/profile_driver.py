#!/usr/bin/env python3
import argparse,json,os,sys,subprocess,threading,time
from pathlib import Path
ROOT=Path('/home/zjh/XiangMu/TradeSystem-rebuild')
sys.path.insert(0,str(ROOT/'record/02_reproducible_measurement'))
import tuning_load

def observe(pid,stop,rows,network):
    os.sched_setaffinity(0,{5})
    last_ss=0
    while not stop.is_set() and Path(f'/proc/{pid}').exists():
        stamp=time.perf_counter_ns()
        entry={'time_ns':stamp,'pid':pid,'threads':[]}
        for task in Path(f'/proc/{pid}/task').glob('*'):
            try:
                stat=(task/'stat').read_text();fields=stat[stat.rfind(')')+2:].split()
                status=(task/'status').read_text()
                switches={s.split(':')[0]:int(s.split(':')[1]) for s in status.splitlines() if s.startswith(('voluntary_ctxt_switches:','nonvoluntary_ctxt_switches:'))}
                t={'tid':int(task.name),'state':fields[0],'minflt':int(fields[7]),'majflt':int(fields[9]),'user_ticks':int(fields[11]),'sys_ticks':int(fields[12]),'cpu':int(fields[36]),'switches':switches,
                   'schedstat':list(map(int,(task/'schedstat').read_text().split())),'wchan':(task/'wchan').read_text().strip()}
                try:t['syscall']=(task/'syscall').read_text().strip()
                except PermissionError:t['syscall']='permission denied'
                entry['threads'].append(t)
            except (FileNotFoundError,ProcessLookupError):pass
        try:entry['io']=(Path(f'/proc/{pid}')/'io').read_text()
        except (FileNotFoundError,ProcessLookupError):pass
        rows.append(entry)
        if stamp-last_ss>1_000_000_000:
            r=subprocess.run(['ss','-tinp'],capture_output=True,text=True)
            network.append({'time_ns':stamp,'exit_code':r.returncode,'stdout':r.stdout,'stderr':r.stderr})
            last_ss=stamp
        stop.wait(.05)

def main():
    p=argparse.ArgumentParser();p.add_argument('--binary',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--strace',action='store_true');p.add_argument('--requests',type=int,default=1000)
    p.add_argument('--rate',type=int,default=100);p.add_argument('--snapshot-ms',type=int,default=60000)
    args=p.parse_args();original=subprocess.Popen;stop=threading.Event();samples=[];network=[];threads=[]
    def launch(command,*a,**kw):
        server=isinstance(command,list) and command and command[0]=='taskset' and str(args.binary.resolve()) in command
        if server and args.strace:
            command=['strace','-D','-qq','-f','-ttt','-T','-e','trace=fsync','-o',str(args.output/'fsync.strace')]+command
        child=original(command,*a,**kw)
        if server:
            thread=threading.Thread(target=observe,args=(child.pid,stop,samples,network),daemon=True);threads.append(thread);thread.start()
        return child
    subprocess.Popen=launch
    cpus=sorted(os.sched_getaffinity(0))
    cfg=dict(mode='open',clients=4,tickers=8,rate=args.rate,requests=args.requests,seed=20260928,timeout_s=60,snapshot_ms=args.snapshot_ms)
    try:result=tuning_load.run(args.binary.resolve(),args.output,cfg,cpus[:4],[cpus[4]])
    finally:
        stop.set()
        for t in threads:t.join()
        subprocess.Popen=original
    (args.output/'thread_samples.json').write_text(json.dumps(samples,indent=2)+'\n')
    (args.output/'tcp_samples.json').write_text(json.dumps(network,indent=2)+'\n')
    (args.output/'observation.json').write_text(json.dumps({'sampler_cpu':5,'sampling_interval_ms':50,'trace_fsync':args.strace,'perturbed_diagnostic_only':True,'command':sys.argv,'python_clock':vars(time.get_clock_info('perf_counter'))},indent=2)+'\n')
    print(result['correctness_ok'],result.get('failure',''),result.get('statistics',{}).get('scheduled_to_response'),flush=True)
    if not result['correctness_ok']:raise SystemExit(1)
if __name__=='__main__':main()
