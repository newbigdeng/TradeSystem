import argparse, importlib.util, json, os, pathlib, re, signal, socket, subprocess, time

def healthy_network(result):
    for cid in ('1','2'):
        recovery=result['recovery_logs'].get(cid,[])
        if not any(line.startswith('RECOVERY PUBLISHED ') for line in recovery): return False
        counters=[line for line in recovery if line.startswith('RECOVERY COUNTERS ')]
        if not counters: return False
        values={key:int(value) for key,value in re.findall(r'(\w+):(\d+)',counters[-1])}
        if values.get('recoveries',0)<1: return False
        if any(values.get(key,-1)!=0 for key in ('gaps','duplicates','cache_overflow','bad_hash','queue_full')): return False
        gateway=[line for line in result['gateway_logs'].get(cid,[]) if line.startswith('GATEWAY STATS ')]
        if not gateway: return False
        values={key:int(value) for key,value in re.findall(r'(\w+):(\d+)',gateway[-1])}
        if values.get('healthy')!=1 or values.get('pending')!=0 or values.get('unsent_bytes')!=0: return False
    return True

def run(repo, binary_dir, output, orders, delay_us, seconds):
    output.mkdir(parents=True, exist_ok=False)
    with socket.socket() as probe:
        if probe.connect_ex(('127.0.0.1', 12345)) == 0:
            raise RuntimeError('port 12345 occupied; existing process preserved')
    env = dict(os.environ, TRADE_SNAPSHOT_MS='500', TRADE_RUN_SECONDS=str(seconds),
               TRADE_RANDOM_ORDERS=str(orders), TRADE_RANDOM_DELAY_US=str(delay_us))
    result = {'config': {k:v for k,v in env.items() if k.startswith('TRADE_')},
              'binary_dir': str(binary_dir), 'started_ns': time.time_ns()}
    processes, logs = [], []
    def launch(name, args, directory):
        directory.mkdir(); log = (directory/'process.log').open('w'); logs.append(log)
        p = subprocess.Popen([str(binary_dir/name), *args], cwd=directory, env=env,
                             stdout=log, stderr=subprocess.STDOUT)
        processes.append(p); return p
    try:
        server = launch('exchange_main', [], output/'exchange')
        for _ in range(300):
            if server.poll() is not None: raise RuntimeError('exchange startup failed')
            with socket.socket() as probe:
                if probe.connect_ex(('127.0.0.1',12345)) == 0: break
            time.sleep(.1)
        else: raise RuntimeError('exchange startup timeout')
        clients = [launch('trading_main', [str(cid),'RANDOM'], output/f'client_{cid}') for cid in (1,2)]
        result['client_exits'] = [p.wait(timeout=seconds+40) for p in clients]
        if server.poll() is None: server.send_signal(signal.SIGTERM)
        result['server_exit'] = server.wait(timeout=30)
    except Exception as ex:
        result['runner_error'] = str(ex)
    finally:
        for p in reversed(processes):
            if p.poll() is None:
                p.send_signal(signal.SIGTERM)
                try: p.wait(timeout=15)
                except subprocess.TimeoutExpired: p.kill(); p.wait()
        for f in logs: f.close()
    spec = importlib.util.spec_from_file_location('reconcile', repo/'record/01_reliability/reconcile.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    checkpoints = [output/f'client_{cid}'/f'trading_account_{cid}.journal' for cid in (1,2)]
    journal = output/'exchange/exchange_orders.journal'
    try:
        result['reconciliation'] = module.reconcile(journal, [p for p in checkpoints if p.exists()])
        result['fill_response_count'] = sum(l.startswith('RESPONSE ') and l.split()[6]=='3' for l in journal.read_text().splitlines())
    except Exception as ex: result['reconciliation_error'] = str(ex)
    result['recovery_logs'] = {}
    result['gateway_logs'] = {}
    for cid in (1,2):
        for key, stem, words in [('recovery_logs','market_data_consumer',('RECOVERY PUBLISHED','RECOVERY COUNTERS')),
                                 ('gateway_logs','order_gateway',('GATEWAY STATS','ORDER SESSION UNKNOWN','TCP terminal'))]:
            p = output/f'client_{cid}'/f'trading_{stem}_{cid}.log'
            result[key][str(cid)] = [l for l in p.read_text().splitlines() if l.startswith(words)] if p.exists() else []
    rec = result.get('reconciliation',{})
    result['correctness_ok'] = (result.get('client_exits')==[0,0] and result.get('server_exit')==0
        and rec.get('consistent',False) and len(rec.get('checkpoints',[]))==2
        and all(c['healthy'] for c in rec['checkpoints']) and result.get('fill_response_count',0)>0
        and healthy_network(result))
    result['ended_ns'] = time.time_ns()
    (output/'result.json').write_text(json.dumps(result,indent=2))
    print(json.dumps({'output':str(output),'ok':result['correctness_ok'],'exits':result.get('client_exits'),
          'received':rec.get('received_count'),'applied':rec.get('applied_count'),'fills':result.get('fill_response_count'),
          'errors':rec.get('errors'),'gateway':result['gateway_logs']},indent=2),flush=True)
    return result

if __name__ == '__main__':
    p=argparse.ArgumentParser()
    p.add_argument('--repo',type=pathlib.Path,required=True)
    p.add_argument('--binary-dir',type=pathlib.Path,required=True)
    p.add_argument('--output',type=pathlib.Path,required=True)
    p.add_argument('--orders',type=int,default=40)
    p.add_argument('--delay-us',type=int,default=100000)
    p.add_argument('--seconds',type=int,default=24)
    a=p.parse_args()
    if not (0<a.orders<=900000 and 0<=a.delay_us<=1000000 and 0<a.seconds<=86400): p.error('settings out of bounds')
    result=run(a.repo.resolve(),a.binary_dir.resolve(),a.output.resolve(),a.orders,a.delay_us,a.seconds)
    raise SystemExit(0 if result['correctness_ok'] else 1)
