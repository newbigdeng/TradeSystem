#!/usr/bin/env python3
import csv,json,os,selectors,socket,sys,threading,time,multiprocessing,hashlib
from pathlib import Path
ROOT=Path('/home/zjh/XiangMu/TradeSystem-rebuild');BASE=Path('/home/zjh/trade-measurements/03')
sys.path.insert(0,str(ROOT/'record/02_reproducible_measurement'))
from load_generator import Peer,requests,drive,REQUEST,RESPONSE,EPOCH,FIELDS
from analyze import analyze
def echo(sockets,stop):
    os.sched_setaffinity(0,{5});selector=selectors.DefaultSelector();state={}
    for s in sockets:s.setblocking(False);selector.register(s,selectors.EVENT_READ);state[s]=[bytearray(),bytearray()]
    rid=0
    try:
        while not stop.is_set():
            for key,mask in selector.select(.001):
                s=key.fileobj;incoming,outgoing=state[s]
                if mask&selectors.EVENT_READ:
                    try:data=s.recv(65536)
                    except BlockingIOError:continue
                    if not data:stop.set();break
                    incoming.extend(data)
                    while len(incoming)>=REQUEST.size:
                        r=REQUEST.unpack(incoming[:REQUEST.size]);del incoming[:REQUEST.size];rid+=1
                        outgoing.extend(RESPONSE.pack(b'TS',1,2,r[3],EPOCH,r[5],r[6],r[7],r[8],r[8],r[9],r[10],0 if r[5]==1 else 0xFFFFFFFF,r[11],0,rid,0))
                    selector.modify(s,selectors.EVENT_READ|selectors.EVENT_WRITE)
                if mask&selectors.EVENT_WRITE:
                    try:n=s.send(outgoing)
                    except BlockingIOError:continue
                    del outgoing[:n]
                    if not outgoing:selector.modify(s,selectors.EVENT_READ)
    finally:selector.close()
def main():
    output=BASE/'generator_separate_process';output.mkdir(exist_ok=False);results=[]
    for iteration in range(5):
        for rate in ([100,1000] if iteration%2==0 else [1000,100]):
            os.sched_setaffinity(0,{4});clients={};servers=[]
            for cid in range(80,84):a,b=socket.socketpair();a.setblocking(False);clients[cid]=Peer(a,cid);servers.append(b)
            context=multiprocessing.get_context('fork');stop=context.Event();worker=context.Process(target=echo,args=(servers,stop));worker.start()
            source=requests(rate*10,4,8,20260928)
            payload=json.dumps(source,sort_keys=True,separators=(',',':'))+'\n'
            try:rows,start,end,counts=drive(clients,source,'open',rate,10)
            finally:stop.set();worker.join()
            for p in clients.values():p.socket.close()
            for s in servers:s.close()
            d=output/f'{iteration}_r{rate}';d.mkdir()
            (d/'input.json').write_text(payload)
            with (d/'events.csv').open('w',newline='') as f:w=csv.DictWriter(f,fieldnames=FIELDS);w.writeheader();w.writerows(rows)
            stats=analyze(d/'events.csv',start,end)
            item={'iteration':iteration,'rate':rate,'counts':counts,'statistics':stats,'scope':'same generator drive against ideal canonical-wire socketpair echo; no TradeSystem matching/audit','generator_cpu':4,'echo_cpu':5,'echo_process_exit':worker.exitcode,'echo_separate_process':True,'input_sha256':hashlib.sha256(payload.encode()).hexdigest(),'seed':20260928,'directory':str(d)}
            results.append(item);(output/'runs.json').write_text(json.dumps(results,indent=2)+'\n')
            print('generator',iteration,rate,counts['completed'],stats['send_lateness']['p99_ns'],flush=True)
            if counts['completed']!=rate*10 or worker.exitcode!=0:raise RuntimeError('generator probe failed')
if __name__=='__main__':main()
