#!/usr/bin/env python3
import csv,json,math,re,statistics
from pathlib import Path
BASE=Path('/home/zjh/trade-measurements/03')
def distribution(values):
    if not values:return None
    s=sorted(values)
    return {'n':len(s),'p50_ns':s[math.ceil(.5*len(s))-1],'p95_ns':s[math.ceil(.95*len(s))-1],'p99_ns':s[math.ceil(.99*len(s))-1],'max_ns':s[-1]}
def analyze(directory):
    stages={};special=[];duplicate=0
    for path in directory.glob('stage_*.csv'):
        with path.open() as f:
            for raw in csv.DictReader(f):
                r={k:int(v) for k,v in raw.items()}
                if r['client']==0:special.append(r);continue
                key=f"{r['client']}:{r['order']}:{r['kind']}"
                if r['stage'] in stages.setdefault(key,{}):duplicate+=1
                stages[key][r['stage']]=r
    metas=[json.loads(p.read_text()) for p in directory.glob('stage_*.json')]
    inputs=json.loads((directory/'input.json').read_text())
    with (directory/'events.csv').open() as f:events=list(csv.DictReader(f))
    slices={k:[] for k in ['generator_lateness','client_to_validated_frame_lower','client_to_validated_frame_upper','receive_audit','fifo_accumulate','input_queue','matching_preprocessing','apply_audit','before_response_audit','response_audit','response_publish','response_queue','gateway_format_encode','socket_pending','kernel_to_client_lower','kernel_to_client_upper','matching_service']}
    mapping={'receive_audit':(0,1),'fifo_accumulate':(1,2),'input_queue':(2,11),'matching_preprocessing':(11,3),'apply_audit':(3,4),'before_response_audit':(4,5),'response_audit':(5,6),'response_publish':(6,7),'response_queue':(7,8),'gateway_format_encode':(8,9),'socket_pending':(9,10),'matching_service':(11,12)}
    rows=[];invalid=[];clock_boundary_overlap=[]
    for e,source in zip(events,inputs):
        key=e['request_id'];s=stages.get(key,{})
        if set(s)!=set(range(14)):
            invalid.append({'request':key,'missing':sorted(set(range(14))-set(s))});continue
        scheduled=int(e['scheduled_ns']);sent=int(e['sent_ns']);completed=int(e['completed_ns'])
        receive_signed=s[0]['time_ns']-sent;send_signed=completed-s[10]['time_ns']
        if receive_signed<0 or send_signed<0:clock_boundary_overlap.append({'request':key,'client_send_return_to_decode_ns':receive_signed,'server_send_return_to_client_ns':send_signed})
        pieces={'generator_lateness':sent-scheduled,'client_to_validated_frame_lower':max(0,receive_signed),
                'client_to_validated_frame_upper':s[0]['time_ns']-int(e['first_write_ns']),
                'kernel_to_client_lower':max(0,send_signed),'kernel_to_client_upper':completed-s[13]['time_ns']}
        pieces.update({name:s[b]['time_ns']-s[a]['time_ns'] for name,(a,b) in mapping.items()})
        if any(v<0 for v in pieces.values()):invalid.append({'request':key,'negative':pieces});continue
        for k,v in pieces.items():slices[k].append(v)
        rows.append({'request_id':key,'ticker':source['ticker'],'kind':e['kind'],'scheduled_ns':scheduled,'sent_ns':sent,'completed_ns':completed,'scheduled_to_response_ns':completed-scheduled,'pieces_ns':pieces,'stages':{str(i):s[i] for i in sorted(s)}})
    slow=sorted(rows,key=lambda r:r['scheduled_to_response_ns'],reverse=True)[:10]
    r=json.loads((directory/'result.json').read_text());start=r['statistics']['window_start_ns'];end=r['statistics']['window_end_ns']
    samples=json.loads((directory/'thread_samples.json').read_text())
    sampled={};wchan={}
    for entry in samples:
        if start<=entry['time_ns']<r['counts']['observed_end_ns']:
            for t in entry['threads']:
                sampled.setdefault(t['tid'],[]).append(t)
                if t['state']!='R':wchan[t['wchan']]=wchan.get(t['wchan'],0)+1
    deltas={}
    for tid,items in sampled.items():
        first,last=items[0],items[-1]
        deltas[tid]={'samples':len(items),'user_tick_delta':last['user_ticks']-first['user_ticks'],'sys_tick_delta':last['sys_ticks']-first['sys_ticks'],
                    'minor_fault_delta':last['minflt']-first['minflt'],'major_fault_delta':last['majflt']-first['majflt'],
                    'voluntary_context_switch_delta':last['switches']['voluntary_ctxt_switches']-first['switches']['voluntary_ctxt_switches'],
                    'nonvoluntary_context_switch_delta':last['switches']['nonvoluntary_ctxt_switches']-first['switches']['nonvoluntary_ctxt_switches'],
                    'cpu_run_ns_delta':last['schedstat'][0]-first['schedstat'][0],'runqueue_wait_ns_delta':last['schedstat'][1]-first['schedstat'][1],
                    'observed_cpu_changes':sum(a['cpu']!=b['cpu'] for a,b in zip(items,items[1:]))}
    out={'directory':str(directory),'trace_only_not_formal_performance':True,'correctness_ok':r['correctness_ok'],'matched_requests':len(rows),'invalid_requests':invalid,'duplicate_stage_events':duplicate,
         'recording_overflow':sum(m['overflow'] for m in metas),'clock_boundary_overlap':clock_boundary_overlap,
         'stage_distributions':{k:distribution(v) for k,v in slices.items()},'slow_requests':slow,
         'snapshot_events':special,'thread_deltas_coarse_sampled':deltas,'blocked_wchan_sample_counts':wchan,
         'clock_contract':'CLOCK_MONOTONIC/steady/Python perf_counter checked separately; no wall-clock subtraction',
         'publish_boundary':'timestamp just before successful queue publish attempt; includes slot copy/index publication bias',
         'matching_service':'first matching peek -> processing returns; may overlap ACK pipeline; never add it to response stages',
         'kernel_acceptance_uncertainty':'send entry/return bracket internal kernel acceptance. Peer can observe before sender returns; signed overlaps retained and network intervals reported as lower/upper bounds'}
    (directory/'stage_analysis.json').write_text(json.dumps(out,indent=2)+'\n')
    print(directory.name,len(rows),'invalid',len(invalid),'overflow',out['recording_overflow'],flush=True)
    if invalid or duplicate or out['recording_overflow']:raise RuntimeError('invalid stage capture retained')
def fsync(directory):
    text=(directory/'fsync.strace').read_text();values=[];errors=[]
    for line in text.splitlines():
        m=re.search(r'fsync\(.*?\)\s+=\s+0\s+<([0-9.]+)>',line)
        if m:values.append(int(float(m.group(1))*1e9))
        elif 'resumed>' in line:
            m=re.search(r'=\s+0\s+<([0-9.]+)>',line)
            if m:values.append(int(float(m.group(1))*1e9))
        elif 'fsync(' in line and 'unfinished' not in line:errors.append(line)
    out={'successful_fsync':len(values),'duration':distribution(values),'unparsed_or_error_lines':errors,'diagnostic_only':True,'traced_real_child':True}
    (directory/'fsync_analysis.json').write_text(json.dumps(out,indent=2)+'\n')
    print('fsync',out,flush=True)
if __name__=='__main__':
    for name in ['profile_stage_off','profile_stage_full','profile_snapshot_burst']:
        if (BASE/name).exists():analyze(BASE/name)
    fsync(BASE/'profile_fsync')
