from pathlib import Path
import csv,json,math,statistics
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
ROOT=Path(__file__).resolve().parents[1]/'evidence'
OUTPUT=ROOT/'figures';OUTPUT.mkdir(exist_ok=True)
plt.rcParams.update({'svg.fonttype':'path','font.size':10,'axes.spines.top':False,'axes.spines.right':False})
colors={'full':'#bb4747','sample64':'#dd9730','off':'#218e83'}
runs=json.loads((ROOT/'paired_runs.json').read_text())
fig,axes=plt.subplots(1,2,figsize=(12,4.4),layout='constrained')
for mode in ['full','sample64','off']:
    values=sorted([r for r in runs if r['variant']==mode],key=lambda r:r['iteration'])
    axes[0].plot(range(1,6),[r['statistics']['all_offered_p99_ns']/1e6 for r in values],'-o',label=mode,color=colors[mode])
    axes[1].plot(range(1,6),[r['statistics']['send_lateness']['p99_ns']/1e6 for r in values],'-o',label=mode,color=colors[mode])
axes[0].axhline(20,color='#4d5563',linestyle='--',label='20 ms budget')
axes[0].set_yscale('log');axes[0].set(title='All five rounds retained',ylabel='Scheduled-to-ACK P99 (ms)',xlabel='Paired round')
axes[1].set(title='Generator lateness recorded separately',ylabel='Full-send lateness P99 (ms)',xlabel='Paired round')
for ax in axes:ax.set_xticks(range(1,6));ax.grid(alpha=.18);ax.legend(fontsize=8)
fig.savefig(OUTPUT/'paired_p99.svg');fig.savefig(ROOT.parent/'preview_paired.png',dpi=120);plt.close(fig)

stage=json.loads((ROOT/'profile_stage_off/stage_analysis.json').read_text());slow=stage['slow_requests'][0]
names=[('scheduled','scheduled_ns'),('send return','sent_ns')]
points=[(0,'Scheduled'),(slow['sent_ns']-slow['scheduled_ns'],'Client send return')]
stage_names={0:'Frame validated',1:'RECEIVED audit done',2:'Input queue publish attempt',11:'Matching dequeue',3:'Process entry',4:'APPLY audit done',5:'Response audit begin',6:'Response audit done',7:'Response queue publish',8:'Gateway dequeue',9:'User send buffer accepted',10:'Send syscall returned'}
order=[0,1,2,11,3,4,5,6,7,8,9,10]
for i in order:points.append((slow['stages'][str(i)]['time_ns']-slow['scheduled_ns'],stage_names[i]))
points.append((slow['completed_ns']-slow['scheduled_ns'],'Client observed ACK'))
fig,ax=plt.subplots(figsize=(12,5.8),layout='constrained')
for i in range(len(points)-1):
    start,label=points[i];end,next_label=points[i+1]
    color='#bb4747' if label=='Input queue publish attempt' else '#218e83'
    ax.barh(i,(end-start)/1e6,left=start/1e6,height=.66,color=color)
    ax.text(end/1e6+12,i,f'{(end-start)/1e6:.3f} ms',va='center',fontsize=8)
ax.set_yticks(range(len(points)-1),[f'{points[i][1]} → {points[i+1][1]}' for i in range(len(points)-1)])
ax.invert_yaxis();ax.set_xlim(0,slow['scheduled_to_response_ns']/1e6*1.1)
ax.set(xlabel='Elapsed from scheduled send (ms)',title=f"Diagnostic trace: request {slow['request_id']} — {slow['scheduled_to_response_ns']/1e6:.3f} ms")
ax.grid(axis='x',alpha=.18);fig.savefig(OUTPUT/'slow_request.svg');fig.savefig(ROOT.parent/'preview_slow.png',dpi=120);plt.close(fig)

data=json.loads((ROOT/'profile_snapshot_burst/stage_analysis.json').read_text())
with (ROOT/'profile_snapshot_burst/events.csv').open() as f:events=list(csv.DictReader(f))
result=json.loads((ROOT/'profile_snapshot_burst/result.json').read_text());start=result['statistics']['window_start_ns']
values=[]
for second in range(10):
    a=[(int(r['completed_ns'])-int(r['scheduled_ns']))/1e6 for r in events if start+second*1e9<=int(r['scheduled_ns'])<start+(second+1)*1e9]
    a.sort();values.append(a[math.ceil(.99*len(a))-1])
fig,ax=plt.subplots(figsize=(11,3.8),layout='constrained');ax.plot([s+.5 for s in range(10)],values,'-o',color='#218e83',label='P99 for scheduled 1s cohort')
cycles={}
for e in data['snapshot_events']:cycles.setdefault(e['order'],{})[e['stage']]=e
for v in cycles.values():
    if set(v)!={14,15}:continue
    left=(v[14]['time_ns']-start)/1e9;right=(v[15]['time_ns']-start)/1e9
    if 0<=left<10:ax.axvspan(left,right,color='#dd9730',alpha=.25)
ax.set(xlim=(0,10),xlabel='Scheduled measurement second',ylabel='Scheduled-to-ACK P99 (ms)',title='Diagnostic only: snapshot intervals shaded; no average of per-second P99')
ax.grid(alpha=.18);ax.legend(fontsize=8);fig.savefig(OUTPUT/'snapshot_timeline.svg');fig.savefig(ROOT.parent/'preview_snapshot.png',dpi=120);plt.close(fig)

overlap=[]
for name in ['profile_stage_off','profile_stage_full','profile_snapshot_burst']:
    d=json.loads((ROOT/name/'stage_analysis.json').read_text());s=d['slow_requests'][0];cycles={}
    for e in d['snapshot_events']:cycles.setdefault(e['order'],{})[e['stage']]=e
    found=[]
    for cycle,v in cycles.items():
        if set(v)=={14,15} and v[14]['time_ns']<s['completed_ns'] and v[15]['time_ns']>s['scheduled_ns']:found.append(cycle)
    overlap.append({'profile':name,'slow_request':s['request_id'],'checked_cycles':len(cycles),'overlapping_cycles':found,'test':'start < request_end AND end > request_start'})
(ROOT/'diagnostic_snapshot_overlap.json').write_text(json.dumps(overlap,indent=2)+'\n')
print(OUTPUT)
