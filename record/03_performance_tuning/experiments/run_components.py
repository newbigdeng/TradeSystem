#!/usr/bin/env python3
from pathlib import Path
import json,os,subprocess
BASE=Path('/home/zjh/trade-measurements/03');ROOT=Path('/home/zjh/XiangMu/TradeSystem-rebuild')
def main():
    output=BASE/'components';output.mkdir(exist_ok=False)
    specs=[('queue','component_probe',['queue']),('tcp','component_probe',['tcp']),('snapshot','component_probe',['snapshot']),
           ('book_cases','book_probe',[]),('book_density','book_scenarios',[])]
    results=[]
    for iteration in range(6):
        for name,binary,args in specs:
            d=output/f'{iteration}_{name}';d.mkdir()
            command=['taskset','-c','0,1',str(BASE/binary)]+args
            if iteration%2 and binary in {'book_probe','book_scenarios'}:command+=['--reverse']
            r=subprocess.run(command,cwd=d,capture_output=True,text=True,timeout=180)
            (d/'stdout.txt').write_text(r.stdout);(d/'stderr.txt').write_text(r.stderr)
            data=[json.loads(s) for s in r.stdout.splitlines() if s.startswith('{')]
            item={'iteration':iteration,'warmup':iteration==0,'control':name,'command':command,'exit_code':r.returncode,'data':data,'directory':str(d)}
            results.append(item);(output/'runs.json').write_text(json.dumps(results,indent=2)+'\n')
            print('component',iteration,name,r.returncode,flush=True)
            if r.returncode:raise RuntimeError('component failure preserved')
    print('COMPLETE components',flush=True)
if __name__=='__main__':main()
