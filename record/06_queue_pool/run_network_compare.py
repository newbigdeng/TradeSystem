#!/usr/bin/env python3
"""Alternate two exchange binaries using the existing audited load generator."""
import argparse,json,pathlib,sys

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--baseline',type=pathlib.Path,required=True)
parser.add_argument('--final',type=pathlib.Path,required=True)
parser.add_argument('--output',type=pathlib.Path,required=True)
args=parser.parse_args()
args.baseline=args.baseline.resolve();args.final=args.final.resolve();args.output=args.output.resolve()
if not args.baseline.is_file() or not args.final.is_file():parser.error('both binaries must exist')
repo=pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0,str(repo/'record/02_reproducible_measurement'))
import load_generator

args.output.mkdir(parents=True,exist_ok=False)
config={'clients':4,'tickers':8,'seed':20260928,'requests':512,'mode':'open','rate':50,'timeout_s':10,'pause_ms':0}
results=[]
for i in range(3):
    for label in (['final','baseline'] if i%2==0 else ['baseline','final']):
        result=load_generator.run(getattr(args,label),args.output/f'{i}_{label}',config,[0,1,2,3],[4])
        results.append({'round':i,'version':label,'result':result})
        (args.output/'network.json').write_text(json.dumps(results,indent=2)+'\n')
        print(i,label,result['correctness_ok'],result['statistics']['actual_send_to_response'],flush=True)
raise SystemExit(0 if all(row['result']['correctness_ok'] for row in results) else 1)
