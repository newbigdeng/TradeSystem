import pathlib,sys,json,subprocess,time
r=pathlib.Path('/home/zjh/XiangMu/TradeSystem-rebuild');e=pathlib.Path('/home/zjh/trade-measurements/08_audit_decomposition');out=e/'indexer-control';out.mkdir(exist_ok=False)
sys.path.insert(0,str(r/'record/02_reproducible_measurement'));import load_generator as g
binary=pathlib.Path('/home/zjh/trade-measurements/07_e2e_latency/build-final/exchange_main')
def state():return subprocess.check_output(['systemctl','--user','show','localsearch-3.service','-p','ActiveState','-p','FreezerState'],text=True)
original=state();(out/'before.txt').write_text(original)
assert 'FreezerState=running' in original and 'ActiveState=active' in original
results=[]
try:
 for phase in ['active','frozen','restored']:
  if phase=='frozen':subprocess.run(['systemctl','--user','freeze','localsearch-3.service'],check=True)
  if phase=='restored':subprocess.run(['systemctl','--user','thaw','localsearch-3.service'],check=True)
  (out/(phase+'.txt')).write_text(state())
  for i in range(2):
   cfg={'clients':4,'tickers':8,'seed':20260928,'requests':256,'mode':'open','rate':50,'timeout_s':20,'pause_ms':0}
   before={n:pathlib.Path('/proc/pressure/'+n).read_text() for n in ['cpu','io','memory']}
   d=g.run(binary,out/f'{phase}-{i}',cfg,[0,1,2,3],[4])
   after={n:pathlib.Path('/proc/pressure/'+n).read_text() for n in ['cpu','io','memory']}
   results.append({'phase':phase,'i':i,'pressure_before':before,'pressure_after':after,'result':d});(out/'results.json').write_text(json.dumps(results,indent=2))
   print(phase,i,d['correctness_ok'],d.get('failure'),d.get('statistics',{}).get('actual_send_to_response'),flush=True)
finally:
 subprocess.run(['systemctl','--user','thaw','localsearch-3.service'],check=True)
 (out/'after.txt').write_text(state())
