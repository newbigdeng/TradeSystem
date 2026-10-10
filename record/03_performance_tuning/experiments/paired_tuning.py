#!/usr/bin/env python3
import json, os, subprocess, sys, time
from pathlib import Path
ROOT=Path('/home/zjh/XiangMu/TradeSystem-rebuild')
BASE=Path('/home/zjh/trade-measurements/03')
sys.path.insert(0,str(ROOT/'record/02_reproducible_measurement'))
from tuning_load import run,sha

def main():
    output=BASE/'paired'
    output.mkdir(exist_ok=False)
    cpus=sorted(os.sched_getaffinity(0))
    binaries={'full':BASE/'baseline_exchange_main','sample64':ROOT/'build/tuning-trace-64/exchange_main','off':ROOT/'build/tuning-trace-0/exchange_main'}
    manifest={'base_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
              'source_diff_sha256':sha(BASE/'candidate_source.diff'),'binaries':{k:{'path':str(v),'sha256':sha(v)} for k,v in binaries.items()},
              'source_state':'full is frozen clean baseline; sample64/off are uncommitted identical source patch with different compile option',
              'comparison':'single change: optional diagnostic timing record policy; critical fsync journal unchanged',
              'protocol':json.loads((BASE/'predeclared_plan.json').read_text()),'started_wall_ns':time.time_ns()}
    (output/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    rows=[]
    for iteration in range(5):
        order=['full','sample64','off']
        order=order[iteration%3:]+order[:iteration%3]
        if iteration%2:order.reverse()
        for mode in order:
            cfg=dict(mode='open',clients=4,tickers=8,rate=100,requests=6000,seed=20260928,timeout_s=60,
                     warm_requests=3000,warm_mode='open',warm_rate=100,snapshot_ms=60000)
            os.sched_setaffinity(0,cpus)
            directory=output/f'{iteration}_{mode}'
            print('START',iteration,mode,flush=True)
            item=run(binaries[mode],directory,cfg,cpus[:4],[cpus[4]])
            item.update(iteration=iteration,variant=mode,directory=str(directory))
            rows.append(item)
            (output/'runs.json').write_text(json.dumps(rows,indent=2)+'\n')
            print('DONE',iteration,mode,'correct',item['correctness_ok'],'p99',item.get('statistics',{}).get('all_offered_p99_ns'),
                  'cpu',item.get('process_cpu_percent_sampled'),'drops',item.get('log_dropped_records'),flush=True)
            if not item['correctness_ok']:raise RuntimeError('failed artifacts retained')
    print('COMPLETE',flush=True)
if __name__=='__main__':main()
