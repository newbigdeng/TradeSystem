#!/usr/bin/env python3
"""Read-only reconciliation of durable simulation journals. Never replays orders."""
import argparse
import collections
import json
from pathlib import Path

def reconcile(path, checkpoints=()):
    journal_path=str(path)
    raw=Path(path).read_text()
    if not raw.endswith('\n'): raise ValueError('incomplete final journal record')
    lines = raw.splitlines()
    if not lines or lines[0] != 'TS_AUDIT_V1':
        raise ValueError('unknown journal version')
    received, applied = collections.Counter(), collections.Counter()
    positions, volume = collections.defaultdict(int), collections.defaultdict(int)
    live, seen, errors = {}, set(), []
    counts = collections.Counter()
    for number, line in enumerate(lines[1:], 2):
        fields = line.split()
        if not fields: continue
        kind = fields[0]; counts[kind] += 1
        if kind in ('RECEIVED', 'APPLY'):
            if len(fields) != 10: raise ValueError(f'bad request line {number}')
            (received if kind == 'RECEIVED' else applied)[tuple(map(int, fields[1:]))] += 1
        elif kind == 'RESPONSE':
            if len(fields) != 13: raise ValueError(f'bad response line {number}')
            rid,cid,ticker,oid,moid,typ,side,price,executed,leaves,reason,pos = map(int,fields[1:])
            if not rid: continue # Transport rejections are not matching events.
            if rid in seen: errors.append(f'duplicate response ID {rid}')
            seen.add(rid); key=(cid,ticker,oid); account=(cid,ticker)
            if typ == 1:
                if key in live: errors.append(f'duplicate acceptance {key}')
                live[key]={'side':side,'price':price,'leaves':leaves,'market_order_id':moid}
            elif typ == 3:
                old=live.get(key)
                if old is None or old['side']!=side or executed<=0 or executed+leaves!=old['leaves']:
                    errors.append(f'fill conservation mismatch at response {rid}')
                positions[account]+=side*executed; volume[account]+=executed
                if leaves and old is not None: old['leaves']=leaves
                else: live.pop(key,None)
            elif typ == 2: live.pop(key,None)
            if pos != positions[account]: errors.append(f'position mismatch at response {rid}')
        else: raise ValueError(f'unknown exchange record at line {number}: {kind}')
    missing = received-applied; extra=applied-received
    if extra: errors.append('applied requests without matching durable RECEIVED records')
    checkpoint_results=[]
    for path in checkpoints:
        rows=Path(path).read_text().splitlines()
        if not rows or rows[0]!='TS_AUDIT_V1': raise ValueError('unknown checkpoint version')
        cid=None; unresolved=0; healthy=False
        for row in rows[1:]:
            f=row.split()
            if f[0]=='ACCOUNT': cid=int(f[1]); healthy=bool(int(f[3]))
            elif f[0]=='POSITION':
                if cid is None: raise ValueError('POSITION before ACCOUNT')
                ticker,pos,vol=map(int,f[1:]); account=(cid,ticker)
                if pos!=positions[account] or vol!=volume[account]: errors.append(f'checkpoint mismatch {account}')
            elif f[0]=='UNRESOLVED': unresolved+=1
            else: raise ValueError('unknown checkpoint record')
        remaining=sum(1 for k in live if k[0]==cid)
        if remaining!=unresolved: errors.append(f'checkpoint unresolved count differs for client {cid}')
        checkpoint_results.append({'client':cid,'healthy':healthy,'unresolved':unresolved,'path':str(path)})
    return {'journal':journal_path,
            'counts':dict(counts),'distinct_response_ids':len(seen),
            'received_count':sum(received.values()),'applied_count':sum(applied.values()),
            'unapplied_requests':[{'request':list(k),'count':v} for k,v in missing.items()],
            'live_orders':[{'client':k[0],'ticker':k[1],'order':k[2],**v} for k,v in sorted(live.items())],
            'positions':{f'{k[0]}:{k[1]}':v for k,v in sorted(positions.items())},
            'volume':{f'{k[0]}:{k[1]}':v for k,v in sorted(volume.items())},
            'checkpoints':checkpoint_results,'errors':errors,
            'consistent':not errors and not missing}

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('journal'); parser.add_argument('checkpoints',nargs='*'); parser.add_argument('--output')
    args=parser.parse_args(); result=reconcile(args.journal,args.checkpoints)
    text=json.dumps(result,indent=2)+'\n'
    if args.output: Path(args.output).write_text(text)
    print(text,end=''); raise SystemExit(0 if result['consistent'] else 1)
