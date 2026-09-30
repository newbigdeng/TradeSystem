#!/usr/bin/env python3
"""Real processes: two clients, late recovery, protocol faults, query and stop."""
import json, os, re, signal, socket, struct, subprocess, tempfile, time
from pathlib import Path
from reconcile import reconcile
ROOT=Path(__file__).resolve().parents[2]
BUILD=ROOT/'build/reliability-release'
REQ=struct.Struct('!2sBBQQBIIQbqI'); RESP=struct.Struct('!2sBBQQBIIQQbqIIBQq')

def connect(process):
    deadline=time.monotonic()+20
    while time.monotonic()<deadline:
        if process.poll() is not None: raise RuntimeError('exchange failed to start')
        peer=socket.socket(); peer.settimeout(.2)
        try:
            peer.connect(('127.0.0.1',12345));peer.settimeout(10);return peer
        except OSError: peer.close();time.sleep(.05)
    raise RuntimeError('exchange start timeout')

def receive(peer):
    data=b''
    while len(data)<RESP.size:
        part=peer.recv(RESP.size-len(data))
        if not part: raise RuntimeError('unexpected EOF')
        data+=part
    return RESP.unpack(data)

def run():
    probe=socket.socket(); occupied=probe.connect_ex(('127.0.0.1',12345))==0;probe.close()
    if occupied: raise RuntimeError('12345 occupied; existing process preserved')
    evidence=Path(tempfile.mkdtemp(prefix='trade-integration-')); exchange_dir=evidence/'exchange';exchange_dir.mkdir()
    env=os.environ.copy();env['TRADE_SNAPSHOT_MS']='200'
    outputs=[];processes=[];peers=[];cases=[]
    def start(binary,args,directory,environment):
        output=open(directory/'process_output.log','w');outputs.append(output)
        p=subprocess.Popen([str(BUILD/binary),*args],cwd=directory,env=environment,stdout=output,stderr=subprocess.STDOUT);processes.append(p);return p
    exchange=start('exchange_main',[],exchange_dir,env)
    try:
        first=connect(exchange);peers.append(first)
        # Bind client 90, then verify rejections do not desynchronize its valid requests.
        seq=1; expected_response=1; epoch=0xABCD90
        def ask(peer,request_seq,cid,kind,oid=1,ticker=0,side=1,price=50,qty=1,version=1,session=epoch,typ=5,reason=0):
            peer.sendall(REQ.pack(b'TS',version,1,request_seq,session,kind,cid,ticker,oid,side,price,qty))
            r=receive(peer)
            assert r[:3]==(b'TS',1,2) and r[5]==typ and r[14]==reason,(r,typ,reason)
            return r
        def case(name,**kw):
            nonlocal expected_response
            r=ask(first,**kw);assert r[3]==expected_response,(name,r)
            expected_response+=1;cases.append({'name':name,'type':r[5],'reason':r[14]});return r
        case('valid_new',request_seq=seq,cid=90,kind=1,typ=1);seq+=1
        case('socket_identity',request_seq=seq,cid=91,kind=1,reason=8)
        case('sequence_gap',request_seq=seq+2,cid=90,kind=1,reason=9)
        case('wrong_session',request_seq=seq,cid=90,kind=1,session=epoch+1,reason=10)
        case('zero_quantity',request_seq=seq,cid=90,kind=1,oid=2,qty=0,reason=5);seq+=1
        case('invalid_ticker',request_seq=seq,cid=90,kind=1,oid=2,ticker=8,reason=1);seq+=1
        case('negative_price',request_seq=seq,cid=90,kind=1,oid=2,price=-1,reason=4);seq+=1
        case('invalid_side',request_seq=seq,cid=90,kind=1,oid=2,side=0,reason=3);seq+=1
        case('invalid_type',request_seq=seq,cid=90,kind=0,oid=2,reason=2);seq+=1
        case('bad_version',request_seq=seq,cid=90,kind=1,version=2,reason=11)
        case('duplicate_economic_id',request_seq=seq,cid=90,kind=1,reason=6);seq+=1
        r=case('query_live',request_seq=seq,cid=90,kind=3,typ=6);seq+=1;assert r[13]==1
        case('cancel',request_seq=seq,cid=90,kind=2,typ=2);seq+=1
        r=case('query_canceled',request_seq=seq,cid=90,kind=3,typ=6);seq+=1;assert r[13]==0
        case('query_missing',request_seq=seq,cid=90,kind=3,oid=999,reason=1);seq+=1
        # Unknown account reconnects read-only with its old epoch; NEW remains blocked.
        first.close();peers.remove(first);time.sleep(.15)
        query=connect(exchange);peers.append(query)
        r=ask(query,1,90,3,typ=6);assert r[3]==1 and r[13]==0
        cases.append({'name':'reconnect_read_only_query','type':r[5],'reason':r[14]})
        r=ask(query,2,90,1,oid=3,reason=10)
        cases.append({'name':'reconnect_new_blocked','type':r[5],'reason':r[14]})
        # Raw invalid client ID must reject without indexing a fixed-size table.
        anonymous=connect(exchange);peers.append(anonymous)
        r=ask(anonymous,1,256,1,session=0xBAD,reason=1)
        cases.append({'name':'invalid_client_id','type':r[5],'reason':r[14]})
        clients=[];client_dirs=[]
        for cid in (1,2):
            directory=evidence/f'client{cid}';directory.mkdir();client_dirs.append(directory)
            client_env=env.copy();client_env.update(TRADE_RUN_SECONDS='4',TRADE_RANDOM_ORDERS='400',TRADE_RANDOM_DELAY_US='1000')
            clients.append(start('trading_main',[str(cid),'RANDOM'],directory,client_env))
            if cid==1: time.sleep(1) # The second client must rebuild an already active book.
        for p in clients: assert p.wait(timeout=25)==0,('client exit',p.returncode,evidence)
        exchange.send_signal(signal.SIGTERM);assert exchange.wait(timeout=15)==0,('exchange exit',exchange.returncode,evidence)
        checkpoints=[d/f'trading_account_{i+1}.journal' for i,d in enumerate(client_dirs)]
        audit=reconcile(exchange_dir/'exchange_orders.journal',checkpoints)
        assert audit['consistent'] and not audit['live_orders'],audit
        stats=[]
        for i,d in enumerate(client_dirs,1):
            engine=(d/f'trading_engine_{i}.log').read_text()
            main=(d/f'trading_main_{i}.log').read_text()
            gw=(d/f'trading_order_gateway_{i}.log')
            if not gw.exists(): gw=d/f'order_gateway_{i}.log'
            verified=re.findall(r'RECOVERY VERIFIED:(\d+)',engine)
            assert verified and '1' in verified,('no verified recovery',d)
            assert 'request_queue:0 response_queue:0 md_queue:0 session_healthy:1' in main,('undrained',main)
            stats.append({'client':i,'recovery_verifications':verified,'gateway_log':str(gw),
                          'checkpoint_healthy':audit['checkpoints'][i-1]['healthy'],
                          'checkpoint_unresolved':audit['checkpoints'][i-1]['unresolved']})
        before=(exchange_dir/'exchange_orders.journal').read_bytes()
        restart=subprocess.run([str(BUILD/'exchange_main')],cwd=exchange_dir,capture_output=True,text=True,timeout=10)
        assert restart.returncode!=0 and (exchange_dir/'exchange_orders.journal').read_bytes()==before
        result={'commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
                'evidence_directory':str(evidence),'protocol_cases':cases,'clients':stats,
                'audit':audit,'exchange_exit':exchange.returncode,'client_exits':[p.returncode for p in clients],
                'restart_refused_exit':restart.returncode,'existing_journal_unchanged':True}
        (ROOT/'record/01_reliability/integration_result.json').write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps(result,indent=2),flush=True)
    finally:
        for peer in peers: peer.close()
        for p in reversed(processes):
            if p.poll() is None: p.kill();p.wait()
        for output in outputs: output.close()

if __name__=='__main__': run()
