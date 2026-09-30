#!/usr/bin/env python3
"""Re-run book/protocol/recovery probes on the checked-out stage of the source."""
import hashlib,json,os,statistics,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parents[2];os.chdir(root)
mode,phase=sys.argv[1:3];baseline='--baseline' in sys.argv
sources={'book':'book_probe.cpp','protocol':'protocol_test.cpp','recovery':'recovery_baseline.cpp' if baseline else 'recovery_test.cpp'}
source=root/'record/01_reliability'/sources[mode];binary=root/'build/reliability-measure'/mode
binary.parent.mkdir(parents=True,exist_ok=True)
flags=['-std=c++20','-O3','-DNDEBUG','-I.','-Iexchange','-Itrading']
if mode=='protocol' and baseline: flags+=['-DTRADE_PROTOCOL_BASELINE']
libraries=[f'build/reliability-release/{name}/liblib{name}.a' for name in ('trading','exchange','common')]
command=['g++',*flags,str(source),*libraries,'-pthread','-o',str(binary)]
subprocess.run(command,check=True)
arguments=['benchmark','array'] if mode=='book' else ['benchmark']
cpu=min(os.sched_getaffinity(0));samples=[]
for i in range(6):
    r=subprocess.run(['taskset','-c',str(cpu),str(binary),*arguments],capture_output=True,text=True,timeout=60,check=True)
    sample=json.loads(next(line for line in r.stdout.splitlines() if line.startswith('{')))
    if mode=='book': sample['memory']=r.stderr
    if i: samples.append(sample)
assert len({s['checksum'] for s in samples})==1
data={'commit':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
      'dirty':bool(subprocess.check_output(['git','status','--porcelain'],text=True)),
      'probe_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),'compile_command':command,
      'cpu_affinity':[cpu],'warmups':1,'repetitions':5,'samples':samples,
      'median_ns_per_operation':statistics.median(s['ns_per_operation'] for s in samples)}
(root/'record/01_reliability'/f'perf_{mode}_{phase}.json').write_text(json.dumps(data,indent=2)+'\n')
print(mode,phase,data['median_ns_per_operation'])
