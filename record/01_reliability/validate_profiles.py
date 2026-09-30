#!/usr/bin/env python3
"""Build diagnostic configurations separately; preserve actual checks and flags."""
import concurrent.futures, hashlib, json, os, re, subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
RECORD=ROOT/'record/01_reliability'
PROFILES={'debug':('Debug',''),'asan':('Debug','address'),'tsan':('Debug','thread')}

def build(name,options):
    builddir=ROOT/f'build/reliability-{name}'
    commands=[['cmake','-S',str(ROOT),'-B',str(builddir),'-G','Ninja',f'-DCMAKE_BUILD_TYPE={options[0]}',f'-DTRADE_SANITIZER={options[1]}'],
              ['cmake','--build',str(builddir),'-j','2']]
    for command in commands:
        r=subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
        print(name,'build',r.returncode,flush=True)
        if r.returncode: raise RuntimeError(r.stdout+r.stderr)
    entries=json.loads((builddir/'compile_commands.json').read_text())
    command=next(e['command'] for e in entries if e['file'].endswith('/exchange_main.cpp'))
    assert '-g ' in command and '-O3' not in command and '-std=' in command,command
    if name=='asan': assert '-fsanitize=address,undefined' in command
    if name=='tsan': assert '-fsanitize=thread' in command and '-fsanitize=address' not in command
    if name in ('asan','tsan'): assert '-O1' in command
    return builddir,command

def main():
    results={}
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as workers:
        builds={name:workers.submit(build,name,options) for name,options in PROFILES.items()}
        configurations={name:job.result() for name,job in builds.items()}
    for name,(directory,compile_command) in configurations.items():
        command=['ctest','--test-dir',str(directory),'--output-on-failure']
        r=subprocess.run(command,cwd=ROOT,capture_output=True,text=True,timeout=900)
        results[name]={'command':command,'exit_code':r.returncode,'compile_command':compile_command,'stdout':r.stdout,'stderr':r.stderr}
        print(name,r.stdout[-1500:],r.stderr[-1500:],flush=True)
    r=subprocess.run(['ctest','--test-dir','build/reliability-release','--output-on-failure'],cwd=ROOT,capture_output=True,text=True,timeout=300)
    results['release']={'exit_code':r.returncode,'stdout':r.stdout,'stderr':r.stderr}
    hashes={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest()
            for folder in ('common','exchange','trading') for p in (ROOT/folder).rglob('*') if p.suffix in ('.cpp','.h')}
    data={'commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
          'dirty':bool(subprocess.check_output(['git','status','--porcelain'],cwd=ROOT,text=True)),
          'source_sha256':hashes,'profiles':results}
    (RECORD/'validation_profiles.json').write_text(json.dumps(data,indent=2)+'\n')
    if any(r['exit_code'] for r in results.values()): raise SystemExit(1)
    print('PASS all four configurations',flush=True)

if __name__=='__main__':main()
