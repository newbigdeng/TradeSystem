#!/usr/bin/env python3
"""Alternate frozen exchange binaries; retain every round and correctness result."""
import argparse,json,pathlib,sys

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--baseline',type=pathlib.Path,required=True)
    ap.add_argument('--final',type=pathlib.Path,required=True)
    ap.add_argument('--output',type=pathlib.Path,required=True)
    ap.add_argument('--rounds',type=int,default=5)
    ap.add_argument('--requests',type=int,default=256)
    ap.add_argument('--rates',type=int,nargs='+',default=[50,500])
    ap.add_argument('--server-cpus',type=int,nargs='+',default=[0,1,2,3])
    ap.add_argument('--generator-cpus',type=int,nargs='+',default=[4])
    args=ap.parse_args()
    if args.rounds<1 or args.requests<2 or args.requests%2 or min(args.rates)<=0:
        ap.error('positive rounds/rates and an even request count >=2 are required')
    for name in ('baseline','final'):
        binary=getattr(args,name).resolve()
        if not binary.is_file():ap.error(f'{name} binary does not exist')
        setattr(args,name,binary)
    repo=pathlib.Path(__file__).resolve().parents[2]
    sys.path.insert(0,str(repo/'record/02_reproducible_measurement'))
    import load_generator as g
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    results=[]
    for pair in range(args.rounds):
        for rate in args.rates:
            for version in (('baseline','final') if pair%2==0 else ('final','baseline')):
                cfg={'clients':4,'tickers':8,'seed':20260928,'requests':args.requests,'mode':'open','rate':rate,'timeout_s':20,'pause_ms':0}
                result=g.run(getattr(args,version),out/f'{pair}-{rate}-{version}',cfg,args.server_cpus,args.generator_cpus)
                result.update(pair=pair,rate=rate,version=version)
                results.append(result)
                (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
                print(pair,rate,version,result['correctness_ok'],result.get('statistics',{}).get('actual_send_to_response'),flush=True)
    return 0 if all(r['correctness_ok'] for r in results) else 1

if __name__=='__main__':raise SystemExit(main())
