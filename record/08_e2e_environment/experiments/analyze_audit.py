import pathlib,csv,json,statistics,math
e=pathlib.Path('/home/zjh/trade-measurements/08_audit_decomposition')
def dist(values):
 values=sorted(values)
 return {'mean_ms':statistics.mean(values)/1e6,'p50_ms':values[math.ceil(len(values)*.5)-1]/1e6,'p99_ms':values[math.ceil(len(values)*.99)-1]/1e6,'max_ms':max(values)/1e6}
summary=[]
for run in sorted(e.glob('[0-9]-*')):
 if not (run/'audit_trace.csv').exists():continue
 phases={}
 for row in csv.DictReader((run/'audit_trace.csv').open()):
  tokens=row['text'].split()
  if tokens[0]=='TS_AUDIT_V1':continue
  if tokens[0]=='RESPONSE':key=f'{tokens[2]}:{tokens[4]}:{tokens[6]}'
  else:key=f'{tokens[1]}:{tokens[6]}:{tokens[4]}'
  assert tokens[0] not in phases.setdefault(key,{})
  phases[key][tokens[0]]={k:int(v) for k,v in row.items() if k!='text'}
 measured=[]
 for row in csv.DictReader((run/'events.csv').open()):
  p=phases[row['request_id']];a,b,c=[p[k] for k in ['RECEIVED','APPLY','RESPONSE']]
  sent,end=int(row['sent_ns']),int(row['completed_ns'])
  d={'request_id':row['request_id'],'total':end-sent,'before_audit':a['start']-sent,
     'between_receive_apply':b['start']-a['synced'],'matching':c['start']-b['synced'],
     'after_durable_response':end-c['synced'],
     'lock_wait':sum(t['locked']-t['start'] for t in [a,b,c]),
     'write':sum(t['written']-t['locked'] for t in [a,b,c]),
     'fsync':sum(t['synced']-t['written'] for t in [a,b,c]),
     'audit_cpu':sum(t['cpu_end']-t['cpu_start'] for t in [a,b,c])}
  for name,t in zip(['received','apply','response'],[a,b,c]):d[name+'_fsync']=t['synced']-t['written']
  assert sum(d[k] for k in ['before_audit','between_receive_apply','matching','after_durable_response','lock_wait','write','fsync'])==d['total']
  measured.append(d)
 fields=[k for k in measured[0] if k!='request_id']
 with (run/'decomposition.csv').open('w',newline='') as out:
  w=csv.DictWriter(out,fieldnames=list(measured[0]));w.writeheader();w.writerows(measured)
 result=json.loads((run/'result.json').read_text())
 item={'run':run.name,'correctness_ok':result['correctness_ok'],'count':len(measured),'segments':{k:dist([d[k] for d in measured]) for k in fields},'negative_segments':{k:sum(d[k]<0 for d in measured) for k in fields}}
 item['slowest_requests']=sorted(measured,key=lambda d:d['total'],reverse=True)[:5]
 assert item['segments']['total']['p50_ms']==result['statistics']['actual_send_to_response']['p50_ns']/1e6
 summary.append(item)
 print(run.name, json.dumps({k:round(item['segments'][k]['p50_ms'],4) for k in fields}),flush=True)
(e/'decomposition.json').write_text(json.dumps(summary,indent=2))
