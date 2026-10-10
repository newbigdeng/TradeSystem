#!/usr/bin/env python3
import argparse, hashlib, json, os, statistics, subprocess, sys, threading, time
from pathlib import Path
ROOT = Path('/home/zjh/XiangMu/TradeSystem-rebuild')
sys.path.insert(0, str(ROOT / 'record/02_reproducible_measurement'))
from tuning_load import run, sha

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--variant', required=True)
    parser.add_argument('--repeats', type=int, default=5)
    parser.add_argument('--rates', default='100')
    parser.add_argument('--seconds', type=int, default=60)
    parser.add_argument('--warm-seconds', type=int, default=30)
    parser.add_argument('--trace', action='store_true')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    cpus = sorted(os.sched_getaffinity(0))
    baseline = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    diff = subprocess.check_output(['git', 'diff', 'HEAD', '--', 'common', 'exchange', 'CMakeLists.txt'], cwd=ROOT)
    (args.output / 'source.diff').write_bytes(diff)
    files = ['common/perf_utils.h','CMakeLists.txt']
    manifest = dict(source_base_commit=baseline, source_patch_sha256=hashlib.sha256(diff).hexdigest(),
                    sources={f:sha(ROOT / f) for f in files}, binary_sha256=sha(args.binary),
                    command=sys.argv, server_cpus=cpus[:4], generator_cpus=[cpus[4]],
                    observation_cpu=cpus[5], trace=args.trace, start_wall_ns=time.time_ns(),
                    primary_metric='scheduled to terminal ACK P99, nearest rank',
                    budget='all correct; P99 <=20 ms; completed within window >=98%',
                    warmup=f'{args.warm_seconds} second open-loop same-rate requests; complete then empty book',
                    window_seconds=args.seconds,
                    experiment_class='short exploratory; not sustained capacity evidence' if args.seconds<60 else '60 second reference/paired measurement',
                    network='loopback; NEW/CANCEL 1:1, noncrossing, clients=4,tickers=8',
                    audit='RECEIVED/APPLY/RESPONSE each write+fsync, unchanged')
    (args.output/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    results = []
    for iteration in range(args.repeats):
        rates=list(map(int,args.rates.split(',')))
        if iteration % 2: rates.reverse()
        for rate in rates:
            config = dict(mode='open', clients=4, tickers=8, rate=rate, requests=rate*args.seconds,
                          seed=20260928, timeout_s=60, warm_requests=rate*args.warm_seconds,
                          warm_mode='open', warm_rate=rate, snapshot_ms=60000)
            if config['requests'] % 8 or config['warm_requests'] % 8:
                raise ValueError('counts must be divisible by 8')
            directory = args.output / f'{iteration}_r{rate}'
            os.sched_setaffinity(0, cpus)
            item=run(args.binary.resolve(), directory, config, cpus[:4], [cpus[4]])
            item.update(variant=args.variant, iteration=iteration, directory=str(directory))
            results.append(item)
            (args.output/'runs.json').write_text(json.dumps(results,indent=2)+'\n')
            s=item.get('statistics',{})
            print(args.variant,iteration,rate,item['correctness_ok'],s.get('scheduled_to_response'),s.get('window_rates_per_s'),item.get('failure',''),flush=True)
            if not item['correctness_ok']:
                raise RuntimeError('failed run preserved; stop')
    print('COMPLETE',args.output,flush=True)

if __name__=='__main__':main()
