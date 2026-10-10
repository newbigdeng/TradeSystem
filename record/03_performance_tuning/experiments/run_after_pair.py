#!/usr/bin/env python3
from pathlib import Path
import json,os,subprocess,zipfile
BASE=Path('/home/zjh/trade-measurements/03');ROOT=Path('/home/zjh/XiangMu/TradeSystem-rebuild')
def logged(name,args,cwd=None):
    with (BASE/(name+'.log')).open('w') as f:
        f.write('COMMAND '+repr(args)+'\n');f.flush()
        r=subprocess.run(args,cwd=cwd,stdout=f,stderr=subprocess.STDOUT)
    print(name,r.returncode,flush=True)
    if r.returncode:raise RuntimeError(name+' failed; log preserved')
def main():
    os.sched_setaffinity(0,set(range(8)))
    libs=[str(x) for x in (ROOT/'build/tuning-trace-0').rglob('*.a')]
    for name in ['component_probe','book_probe','book_scenarios']:
        cmd=['g++','-std=c++20','-O3','-DNDEBUG','-Wall','-Wextra','-Werror','-Wpedantic','-DTRADE_PERF_TRACE_EVERY=0','-I',str(ROOT),'-I',str(ROOT/'exchange'),'-I',str(ROOT/'trading'),str(BASE/(name+'.cpp'))]+libs+['-pthread','-o',str(BASE/name)]
        logged('build_'+name,cmd)
    logged('component_experiments',['python3',str(BASE/'run_components.py')])
    source=BASE/'stage_source'
    with zipfile.ZipFile(BASE/'stage_source.zip') as z:z.extractall(source)
    for mode in [0,1]:
        build=BASE/f'stage_build_{mode}'
        logged(f'configure_stage_{mode}',['cmake','-S',str(source),'-B',str(build),'-G','Ninja','-DCMAKE_BUILD_TYPE=Release',f'-DCMAKE_CXX_FLAGS=-DTRADE_PERF_TRACE_EVERY={mode}'])
        logged(f'build_stage_{mode}',['cmake','--build',str(build),'--target','exchange_main','-j','4'])
    logged('profile_stage_off',['python3',str(BASE/'profile_driver.py'),'--binary',str(BASE/'stage_build_0/exchange_main'),'--output',str(BASE/'profile_stage_off'),'--requests','1000'])
    logged('profile_stage_full',['python3',str(BASE/'profile_driver.py'),'--binary',str(BASE/'stage_build_1/exchange_main'),'--output',str(BASE/'profile_stage_full'),'--requests','1000'])
    logged('profile_fsync',['python3',str(BASE/'profile_driver.py'),'--binary',str(ROOT/'build/tuning-trace-0/exchange_main'),'--output',str(BASE/'profile_fsync'),'--requests','1000','--strace'])
    logged('profile_without_stage',['python3',str(BASE/'profile_driver.py'),'--binary',str(ROOT/'build/tuning-trace-0/exchange_main'),'--output',str(BASE/'profile_without_stage'),'--requests','1000'])
    logged('build_clock_probe',['g++','-std=c++20','-O3','-Wall','-Wextra','-Werror','-Wpedantic','-I',str(source),str(BASE/'clock_probe.cpp'),'-o',str(BASE/'clock_probe')])
    clock=BASE/'clock';clock.mkdir();logged('clock_probe',[str(BASE/'clock_probe')],clock)
    logged('generator_self_probe',['python3',str(BASE/'generator_probe.py')])
    (BASE/'capacity_diagnostic_plan.json').write_text(json.dumps({'classification':'exploratory after primary experiment; cannot certify sustained capacity','rates':[80,100,120,160,200],'repeats':5,'window_seconds':10,'warmup_seconds':2,'binary':'trace off candidate','retention':'all attempts; no outlier filtering','budget':'same 20ms and 98% in-window criterion shown for context; short windows not admission proof'},indent=2)+'\n')
    logged('short_capacity_ladder',['python3',str(BASE/'run_tuning.py'),'--binary',str(ROOT/'build/tuning-trace-0/exchange_main'),'--output',str(BASE/'capacity_diagnostic'),'--variant','off_short_exploratory','--repeats','5','--rates','80,100,120,160,200','--seconds','10','--warm-seconds','2'])
    print('COMPLETE after pair',flush=True)
if __name__=='__main__':main()
