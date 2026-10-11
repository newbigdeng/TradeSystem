import pathlib,csv,json
e=pathlib.Path('/home/zjh/trade-measurements/08_audit_decomposition');summary=json.loads((e/'decomposition.json').read_text());out=[]
for item in summary:
 run=e/item['run'];rows=[]
 for row in csv.DictReader((run/'audit_trace.csv').open()):
  tokens=row['text'].split();row.update({k:int(row[k]) for k in ['start','locked','written','synced']});row['phase']=tokens[0]
  if tokens[0]=='RESPONSE':row['key']=f'{tokens[2]}:{tokens[4]}:{tokens[6]}'
  elif tokens[0]!='TS_AUDIT_V1':row['key']=f'{tokens[1]}:{tokens[6]}:{tokens[4]}'
  else:continue
  rows.append(row)
 events=list(csv.DictReader((run/'events.csv').open()))
 # Examine both the slowest request overall and the longest response-after-durability gap.
 decomp=list(csv.DictReader((run/'decomposition.csv').open()))
 keys={max(decomp,key=lambda d:int(d[k]))['request_id'] for k in ['total','after_durable_response']}
 for key in sorted(keys):
  response=next(t for t in rows if t['phase']=='RESPONSE' and t['key']==key)
  end=int(next(t for t in events if t['request_id']==key)['completed_ns']);start=response['synced']
  overlaps=[{'text':t['text'],'overlap_ms':max(0,min(t['synced'],end)-max(t['start'],start))/1e6,'start':t['start'],'synced':t['synced']} for t in rows if t['phase']=='RECEIVED' and t['start']<end and t['synced']>start]
  result={'run':run.name,'request_id':key,'after_durable_ms':(end-start)/1e6,'received_audit_overlap_ms':sum(t['overlap_ms'] for t in overlaps),'overlaps':overlaps}
  out.append(result);print(json.dumps({k:v for k,v in result.items() if k!='overlaps'}))
(e/'overlap.json').write_text(json.dumps(out,indent=2))
