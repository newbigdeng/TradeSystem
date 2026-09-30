#!/usr/bin/env python3
"""One warmup and five timed runs; refuse failed/checksum-changing runs."""
import hashlib,json,os,platform,statistics,subprocess,sys
from pathlib import Path
root=Path(__file__).resolve().parents[2]
os.chdir(root)
mode,phase=sys.argv[1:3]
flags=['-std=c++20','-O3','-DNDEBUG']
if '--baseline' in sys.argv: flags+=['-DTRADE_BASELINE']
build=root/'build/reliability-measure';build.mkdir(parents=True,exist_ok=True)
binary=build/'benchmark'
subprocess.run(['g++',*flags,'-I.','-Iexchange','record/01_reliability/reliability_benchmark.cpp','common/tcp_socket.cpp','-pthread','-o',str(binary)],check=True)
cpu=min(os.sched_getaffinity(0))
sources={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest()
         for directory in ['common','exchange','trading'] for p in sorted((root/directory).rglob('*'))
         if p.suffix in ['.h','.cpp']}
environment={'commit':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),
             'dirty':bool(subprocess.check_output(['git','status','--porcelain'],text=True)),
             'source_sha256':sources,'kernel':platform.release(),
             'compiler':subprocess.check_output(['g++','--version'],text=True).splitlines()[0],
             'cpu_affinity':[cpu],'warmups':1,'repetitions':5,'compiler_flags':' '.join(flags),
             'note':'valid non-overflow / small-write workload; startup and shutdown excluded from timer'}
samples=[]
for i in range(6):
    p=subprocess.run(['taskset','-c',str(cpu),str(binary),mode],cwd=build,capture_output=True,text=True,timeout=45,check=True)
    sample=json.loads([line for line in p.stdout.splitlines() if line.startswith('{')][-1])
    if i:samples.append(sample)
if len({x['checksum'] for x in samples})!=1:raise RuntimeError('checksums differ')
data={'environment':environment,'samples':samples,
      'median_ns_per_operation':statistics.median(x['ns_per_operation'] for x in samples)}
path=root/'record/01_reliability'/('perf_'+mode+'_'+phase+'.json')
path.write_text(json.dumps(data,indent=2)+'\n')
print(mode,phase,data['median_ns_per_operation'],flush=True)
