#!/usr/bin/env python3
import csv,hashlib,json,math,statistics
from pathlib import Path
BASE=Path('/home/zjh/trade-measurements/03')
def distribution(values):
    s=sorted(values)
    return {'n':len(s),'p50':s[math.ceil(.5*len(s))-1],'p95':s[math.ceil(.95*len(s))-1],'p99':s[math.ceil(.99*len(s))-1],'max':s[-1]}
def across(values):return {'median':statistics.median(values),'min':min(values),'max':max(values),'values':values}
def main():
    rows=json.loads((BASE/'paired/runs.json').read_text())
    groups={}
    for mode in ['full','sample64','off']:
        values=[r for r in rows if r['variant']==mode]
        if not values:continue
        metric={}
        for name in ['scheduled_to_response','actual_send_to_response','send_lateness']:
            metric[name]={m:across([r['statistics'][name][m] for r in values]) for m in ['p50_ns','p95_ns','p99_ns','max_ns']}
        metric['completed_per_s']=across([r['statistics']['window_rates_per_s']['completed'] for r in values])
        metric['cpu_percent']=across([r['process_cpu_percent_sampled'] for r in values])
        metric['request_queue_highwater']=across([r['queue_final_and_highwater'][3] for r in values])
        metric['memory_peak_KiB']=across([int(r['process_after']['VmHWM'].split()[0]) for r in values])
        metric['all_correct']=all(r['correctness_ok'] for r in values)
        metric['all_under_budget']=all(r['statistics']['all_offered_p99_ns']<=20_000_000 and r['statistics']['window_counts']['completed']>=.98*r['config']['requests'] for r in values)
        logs=[]
        for r in values:
            d=Path(r['directory']);stats={}
            for log in d.glob('*.log'):
                n=rdtsc=ttt=0
                with log.open() as f:
                    for line in f:n+=1;rdtsc+=' RDTSC ' in line;ttt+=' TTT ' in line
                stats[log.name]={'bytes':log.stat().st_size,'lines':n,'rdtsc':rdtsc,'ttt':ttt}
            r['diagnostic_log_stats']=stats
            logs.append(sum(s['bytes'] for s in stats.values()))
        metric['diagnostic_bytes']=across(logs)
        groups[mode]=metric
    paired={}
    for mode in ['sample64','off']:
        data=[]
        for i in range(5):
            full=next((r for r in rows if r['iteration']==i and r['variant']=='full'),None)
            candidate=next((r for r in rows if r['iteration']==i and r['variant']==mode),None)
            if full and candidate:
                old=full['statistics']['all_offered_p99_ns'];new=candidate['statistics']['all_offered_p99_ns']
                data.append({'iteration':i,'baseline_p99_ns':old,'candidate_p99_ns':new,'reduction_percent':100*(old-new)/old})
        if data:paired[mode]={'runs':data,'per_pair_percent_reduction':across([r['reduction_percent'] for r in data])}
    out={'scope':'5 repeated 60-second open-loop runs per mode; separate per-run quantiles', 'groups':groups,'paired':paired,
         'measured_requests':sum(r['config']['requests'] for r in rows),'input_hashes':sorted({r['input_sha256'] for r in rows}),
         'all_correct':all(r['correctness_ok'] for r in rows),'all_timeout':sum(r['counts']['timeout'] for r in rows),
         'all_unsent':sum(r['counts']['unsent'] for r in rows),'total_audit_requests_with_warmup':sum(r['reconciliation']['received_count'] for r in rows)}
    (BASE/'paired_summary.json').write_text(json.dumps(out,indent=2)+'\n')
    (BASE/'paired_runs_enriched.json').write_text(json.dumps(rows,indent=2)+'\n')
    print(json.dumps({k:{'p99_ms':v['scheduled_to_response']['p99_ns']['median']/1e6,'cpu':v['cpu_percent']['median'],'log_MiB':v['diagnostic_bytes']['median']/1024**2,'correct':v['all_correct'],'budget':v['all_under_budget']} for k,v in groups.items()},indent=2))
if __name__=='__main__':main()
