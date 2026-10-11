import pathlib,os,time,json,subprocess,math
e=pathlib.Path('/home/zjh/trade-measurements/08_audit_decomposition');out=e/'storage-control';out.mkdir(exist_ok=False)
results=[];os.sched_setaffinity(0,[0])
def state():return subprocess.check_output(['systemctl','--user','show','localsearch-3.service','-p','ActiveState','-p','FreezerState'],text=True)
original=state();assert 'FreezerState=running' in original
try:
 for phase in ['active','frozen','restored']:
  if phase=='frozen':subprocess.run(['systemctl','--user','freeze','localsearch-3.service'],check=True)
  if phase=='restored':subprocess.run(['systemctl','--user','thaw','localsearch-3.service'],check=True)
  values=[]
  fd=os.open(out/(phase+'.data'),os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
  try:
   for i in range(256):
    start=time.perf_counter_ns();cpu=time.thread_time_ns()
    assert os.write(fd,b'x'*128)==128
    os.fsync(fd)
    values.append({'wall_ns':time.perf_counter_ns()-start,'cpu_ns':time.thread_time_ns()-cpu})
  finally:os.close(fd)
  ordered=sorted(v['wall_ns'] for v in values)
  result={'phase':phase,'state':state(),'samples':values,'p50_ms':ordered[127]/1e6,'p99_ms':ordered[math.ceil(256*.99)-1]/1e6,'max_ms':max(ordered)/1e6}
  results.append(result);(out/'results.json').write_text(json.dumps(results,indent=2))
  print(phase,{k:v for k,v in result.items() if k not in ['samples','state']},flush=True)
finally:
 subprocess.run(['systemctl','--user','thaw','localsearch-3.service'],check=True)
 (out/'restored.txt').write_text(state())
