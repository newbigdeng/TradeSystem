import pathlib,sys,json
r=pathlib.Path('/home/zjh/XiangMu/TradeSystem-rebuild');e=pathlib.Path('/home/zjh/trade-measurements/08_audit_decomposition')
sys.path.insert(0,str(r/'record/02_reproducible_measurement'));import load_generator as g
results=json.loads((e/'results.json').read_text())
for round in range(3,5):
 for label in (['baseline','flush'] if round%2==0 else ['flush','baseline']):
  cfg={'clients':4,'tickers':8,'seed':20260928,'requests':256,'mode':'open','rate':500,'timeout_s':20,'pause_ms':0}
  d=g.run(e/(label+'-build/exchange_main'),e/f'{round}-{label}',cfg,[0,1,2,3],[4])
  results.append({'round':round,'label':label,'result':d});(e/'results.json').write_text(json.dumps(results,indent=2))
  print(round,label,d['correctness_ok'],d.get('statistics',{}).get('actual_send_to_response'),flush=True)
