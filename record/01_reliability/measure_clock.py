#!/usr/bin/env python3
import hashlib,json,os,statistics,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parents[2];os.chdir(root);phase=sys.argv[1]
binary=root/'build/reliability-measure/clock_probe'
binary.parent.mkdir(parents=True,exist_ok=True)
subprocess.run(['g++','-std=c++20','-O3','-DNDEBUG','-I.','record/01_reliability/clock_probe.cpp','-o',str(binary)],check=True)
cpu=min(os.sched_getaffinity(0));samples=[]
for i in range(6):
    sample=json.loads(subprocess.check_output(['taskset','-c',str(cpu),str(binary)],text=True))
    if i:samples.append(sample)
data={'commit':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
      'dirty':bool(subprocess.check_output(['git','status','--porcelain'],text=True)),
      'source_sha256':hashlib.sha256((root/'common/time_utils.h').read_bytes()).hexdigest(),
      'flags':'-std=c++20 -O3 -DNDEBUG','cpu_affinity':[cpu],'warmups':1,'repetitions':5,
      'samples':samples,'median_ns_per_operation':statistics.median(x['ns_per_operation'] for x in samples)}
(root/'record/01_reliability'/f'perf_clock_{phase}.json').write_text(json.dumps(data,indent=2)+'\n');print(phase,data['median_ns_per_operation'])
