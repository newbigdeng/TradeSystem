#!/usr/bin/env python3
import csv,hashlib,json,math,statistics,sys,subprocess
from pathlib import Path
ROOT=Path('/home/zjh/XiangMu/TradeSystem-rebuild');BASE=Path('/home/zjh/trade-measurements/03')
sys.path.insert(0,str(ROOT/'record/02_reproducible_measurement'))
from analyze import analyze
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    output=BASE/'publication';output.mkdir(exist_ok=False);index=[]
    def copy(src,dest):
        target=output/dest;target.parent.mkdir(parents=True,exist_ok=True);data=src.read_bytes()
        if src.suffix=='.csv':data=data.replace(b'\r\n',b'\n')
        original=str(src.relative_to(BASE)) if src.is_relative_to(BASE) else str(src)
        target.write_bytes(data);index.append({'original':original,'published':str(target.relative_to(output)),'original_sha256':sha(src),'published_sha256':sha(target),'bytes':target.stat().st_size,'transformation':'CRLF to LF only' if data!=src.read_bytes() else 'byte-identical'})
    for src,dest in [('predeclared_plan.json','experiment_plan.json'),('paired/manifest.json','paired_manifest.json'),('paired_summary.json','paired_summary.json'),('paired_runs_enriched.json','paired_runs.json'),('components/runs.json','component_runs.json'),('capacity_diagnostic_plan.json','capacity_diagnostic_plan.json'),('capacity_diagnostic/runs.json','capacity_runs.json'),('generator_self_probe/runs.json','generator_thread_runs.json'),('generator_separate_process/runs.json','generator_process_runs.json'),('generator_methodology.json','generator_methodology.json'),('final_validation.json','final_validation.json'),('api_machine_code_check.json','api_machine_code_check.json'),('clock_probe.log','validation/clock_probe_v1.txt'),('clock_probe_v2.log','validation/clock_probe_v2.txt'),('clock_methodology.json','clock_methodology.json'),('trace_mode_tests.txt','validation/trace_mode_tests.txt'),('api_probe_before.log','validation/api_probe_before.txt'),('build_component.log','validation/first_component_compile_failure.txt'),('perf_permission.stderr','validation/perf_permission.txt')]:copy(BASE/src,dest)
    for f in BASE.glob('final_*.log'):copy(f,'validation/'+f.stem+'.txt')
    for f in ['env_uname.txt','env_lscpu.txt','env_findmnt.txt','baseline_compile_commands.json']:copy(BASE/f,'environment/'+f)
    copy(ROOT/'build/tuning-trace-0/compile_commands.json','environment/trace_off_compile_commands.json')
    copy(ROOT/'build/tuning-trace-64/compile_commands.json','environment/trace_sample64_compile_commands.json')
    for f in BASE.glob('trace_*_test.log'):copy(f,'validation/'+f.stem+'.txt')
    for f in ['ctest_trace_0.log','ctest_trace_64.log','api_probe_after.log','snapshot_after_api.log']:copy(BASE/f,'validation/'+Path(f).stem+'.txt')
    verified=[]
    for prefix in ['before_change','baseline_smoke','smoke_trace_0','smoke_trace_64','paired','capacity_diagnostic']:
        for f in (BASE/prefix).rglob('result.json'):
            r=json.loads(f.read_text());events=f.parent/'events.csv';s=r['statistics']
            actual=analyze(events,s['window_start_ns'],s['window_end_ns']);assert actual==s,(f,'statistics mismatch')
            assert sha(events)==r['events_sha256'];assert sha(f.parent/'input.json')==r['input_sha256']
            verified.append({'file':str(events.relative_to(BASE)),'rows':s['total_rows'],'exact_reanalysis_match':True,'original_sha256':sha(events)})
            if prefix in {'paired','capacity_diagnostic'}:copy(events,str(events.relative_to(BASE)))
            if prefix in {'before_change','baseline_smoke','smoke_trace_0','smoke_trace_64'}:copy(f,str(f.relative_to(BASE)))
    copy(BASE/'paired/0_full/input.json','primary_input.json')
    for name in ['profile_stage_off','profile_stage_full','profile_snapshot_burst','profile_fsync','profile_without_stage']:
        d=BASE/name;r=json.loads((d/'result.json').read_text());s=r['statistics'];assert analyze(d/'events.csv',s['window_start_ns'],s['window_end_ns'])==s
        verified.append({'file':name+'/events.csv','rows':s['total_rows'],'exact_reanalysis_match':True,'original_sha256':sha(d/'events.csv')})
        for f in d.iterdir():
            if f.name in {'input.json','result.json','events.csv','stage_analysis.json','fsync_analysis.json','fsync.strace','observation.json','thread_samples.json'} or f.name.startswith('stage_'):copy(f,name+'/'+f.name)
        # Keep complete original socket observations locally. Publish only the
        # owned test port, excluding unrelated SSH and other service sockets.
        raw=json.loads((d/'tcp_samples.json').read_text());filtered=[]
        for row in raw:
            kept=[];keep_next=False
            for line in row['stdout'].splitlines():
                if line and not line[0].isspace():keep_next=':12345' in line
                if keep_next:kept.append(line)
            filtered.append(dict(time_ns=row['time_ns'],owned_port=12345,stdout='\n'.join(kept)))
        target=output/name/'tcp_samples_owned_port.json';target.write_text(json.dumps(filtered,indent=2)+'\n')
        index.append({'original':name+'/tcp_samples.json','published':str(target.relative_to(output)),'original_sha256':sha(d/'tcp_samples.json'),'published_sha256':sha(target),'bytes':target.stat().st_size,'transformation':'retain only port 12345 and corresponding details'})
    for prefix in ['generator_self_probe','generator_separate_process']:
        rows=json.loads((BASE/prefix/'runs.json').read_text())
        for r in rows:
            d=Path(r['directory']);s=r['statistics'];assert analyze(d/'events.csv',s['window_start_ns'],s['window_end_ns'])==s
            verified.append({'file':str((d/'events.csv').relative_to(BASE)),'rows':s['total_rows'],'exact_reanalysis_match':True,'original_sha256':sha(d/'events.csv')})
        # One complete trace at each target rate; all ten per-run summaries and
        # hashes remain available, as do the complete originals on the VM.
        for rate in [100,1000]:copy(BASE/prefix/f'0_r{rate}/events.csv',Path(prefix)/f'0_r{rate}/events.csv')
    for r in json.loads((BASE/'components/runs.json').read_text()):
        if r['iteration']!=1:continue
        d=Path(r['directory'])
        for f in d.glob('*.csv'):copy(f,'component_samples/'+d.name+'/'+f.name)
    for f in (BASE/'snapshot_after_api').glob('*.csv'):copy(f,'snapshot_after_api/'+f.name)
    for f in (BASE/'clock').glob('*.csv'):copy(f,'clock/'+f.name)
    for f in (BASE/'clock_v2').glob('*.csv'):copy(f,'clock_v2/'+f.name)
    (output/'reanalysis.json').write_text(json.dumps({'verified_files':len(verified),'all_exact':True,'files':verified},indent=2)+'\n')
    raw=[]
    for f in BASE.rglob('*'):
        relative=f.relative_to(BASE)
        if not f.is_file() or relative.parts[0] in {'publication','stage_source','stage_build_0','stage_build_1','__pycache__'} or f.name=='raw_artifact_index.json':continue
        raw.append({'path':str(relative),'bytes':f.stat().st_size,'sha256':sha(f)})
    raw_index={'root':str(BASE),'policy':'raw logs/CSV/journals/failed outputs/source archives preserved; bulky build trees excluded; source archives and patches are not published','files':raw}
    (BASE/'raw_artifact_index.json').write_text(json.dumps(raw_index,indent=2)+'\n')
    copy(BASE/'raw_artifact_index.json','raw_artifact_index.json')
    (output/'publication_index.json').write_text(json.dumps({'raw_root':str(BASE),'code_not_committed':True,'entries':index,'data_policy':'all primary/ladder request CSV published; representative component/generator samples; all original rounds retained with hashes'},indent=2)+'\n')
    print('PUBLISHED',len(index),'reanalyzed',len(verified),sum(x['rows'] for x in verified),'bytes',sum(f.stat().st_size for f in output.rglob('*') if f.is_file()),flush=True)
if __name__=='__main__':main()
