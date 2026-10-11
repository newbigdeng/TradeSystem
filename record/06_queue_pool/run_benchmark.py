#!/usr/bin/env python3
"""Compile and repeat the queue/pool component benchmark on Linux."""
import argparse,json,pathlib,statistics,subprocess

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--repo',type=pathlib.Path,required=True)
parser.add_argument('--output',type=pathlib.Path,required=True)
parser.add_argument('--rounds',type=int,default=5)
args=parser.parse_args()
if args.rounds<1:parser.error('--rounds must be positive')
args.repo=args.repo.resolve();args.output=args.output.resolve()
args.output.mkdir(parents=True,exist_ok=False)
source=pathlib.Path(__file__).with_name('component_benchmark.cpp').resolve()
binary=args.output/'benchmark'
command=['g++','-std=c++20','-O3','-DNDEBUG','-pthread','-Wall','-Wextra','-Werror','-Wpedantic',
         '-I'+str(args.repo),str(source),'-o',str(binary)]
with (args.output/'compile.log').open('w') as log:
    subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
rounds=[]
for i in range(args.rounds):
    run=subprocess.run([str(binary),'1024'],check=True,text=True,capture_output=True)
    (args.output/f'round_{i}.jsonl').write_text(run.stdout)
    rounds.append([json.loads(line) for line in run.stdout.splitlines()])
(args.output/'results.json').write_text(json.dumps(rounds,indent=2)+'\n')
for sample in rounds[0]:
    mode=sample['mode'];rows=[next(v for v in row if v['mode']==mode) for row in rounds]
    value=statistics.median(v['ns_per_op'] for v in rows) if 'ns_per_op' in sample else sample
    print(mode,value)
