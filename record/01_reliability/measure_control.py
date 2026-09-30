#!/usr/bin/env python3
import json,os,statistics,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parents[2];os.chdir(root)
phase=sys.argv[1];flags=['-std=c++20','-O3','-DNDEBUG']
if '--baseline' in sys.argv:flags+=['-DTRADE_CONTROL_BASELINE']
elif '--feature-baseline' in sys.argv:flags+=['-DTRADE_FEATURE_BASELINE']
binary='build/reliability-measure/control_probe'
subprocess.run(['g++',*flags,'-I.','-Iexchange','-Itrading','record/01_reliability/control_probe.cpp','build/reliability-release/trading/liblibtrading.a','build/reliability-release/exchange/liblibexchange.a','build/reliability-release/common/liblibcommon.a','-pthread','-o',binary],check=True)
cpu=min(os.sched_getaffinity(0));commit=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
if '--baseline' in sys.argv:
    outcomes={}
    for mode in ['state','admission','feature']:
        r=subprocess.run([binary,mode],capture_output=True,text=True,timeout=30)
        outcomes[mode]={'exit_code':r.returncode,'stdout':r.stdout,'stderr':r.stderr};print(mode,r.returncode,r.stderr[-200:],flush=True)
    (root/'record/01_reliability/control_baseline_failures.json').write_text(json.dumps({'commit':commit,'cases':outcomes},indent=2)+'\n')
for mode in [x for x in sys.argv[2:] if not x.startswith('--')] or ['admission_benchmark','feature_benchmark','lifecycle']:
    samples=[]
    for i in range(6):
        r=subprocess.run(['taskset','-c',str(cpu),binary,mode],capture_output=True,text=True,timeout=30,check=True)
        sample=json.loads([x for x in r.stdout.splitlines() if x.startswith('{')][-1])
        if i:samples.append(sample)
    if len({x['checksum'] for x in samples})!=1:raise RuntimeError('checksum mismatch')
    data={'commit':commit,'compiler_flags':' '.join(flags),'cpu_affinity':[cpu],'warmups':1,'repetitions':5,'samples':samples,'median_ns_per_operation':statistics.median(x['ns_per_operation'] for x in samples)}
    (root/'record/01_reliability'/('perf_'+mode+'_'+phase+'.json')).write_text(json.dumps(data,indent=2)+'\n');print(mode,phase,data['median_ns_per_operation'],flush=True)
