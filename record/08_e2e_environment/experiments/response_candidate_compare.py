import pathlib,sys,json,time
r=pathlib.Path('/home/zjh/XiangMu/TradeSystem-rebuild');e=pathlib.Path('/home/zjh/trade-measurements/08_audit_decomposition');out=e/'candidate-paired';out.mkdir(exist_ok=False)
sys.path.insert(0,str(r/'record/02_reproducible_measurement'));import load_generator as g
bins={'baseline':pathlib.Path('/home/zjh/trade-measurements/07_e2e_latency/build-final/exchange_main'),'candidate':e/'response-candidate-build/exchange_main'}
def pressure():return {name:pathlib.Path('/proc/pressure/'+name).read_text() for name in ['cpu','io','memory']}
results=[]
for round in range(3):
 for rate in [50,500]:
  for label in (['baseline','candidate'] if round%2==0 else ['candidate','baseline']):
   cfg={'clients':4,'tickers':8,'seed':20260928,'requests':256,'mode':'open','rate':rate,'timeout_s':20,'pause_ms':0}
   before=pressure();d=g.run(bins[label],out/f'{round}-{rate}-{label}',cfg,[0,1,2,3],[4]);after=pressure()
   results.append({'round':round,'rate':rate,'label':label,'pressure_before':before,'pressure_after':after,'result':d});(out/'results.json').write_text(json.dumps(results,indent=2))
   print(round,rate,label,d['correctness_ok'],d.get('statistics',{}).get('actual_send_to_response'),flush=True)
