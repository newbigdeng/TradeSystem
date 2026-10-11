import ctypes as c,ctypes.wintypes as w,time,json,pathlib,subprocess,sys
p=pathlib.Path(__file__).parent;pd=c.WinDLL('pdh');kernel=c.WinDLL('kernel32',use_last_error=True)
class Value(c.Structure):_fields_=[('status',w.DWORD),('value',c.c_double)]
class Memory(c.Structure):_fields_=[('length',w.DWORD),('load',w.DWORD)]+[(k,c.c_ulonglong) for k in ['total','available','totalPageFile','availablePageFile','totalVirtual','availableVirtual','extended']]
query=w.HANDLE();errors={};handles={}
rc=pd.PdhOpenQueryW(None,0,c.byref(query));assert rc==0,rc
paths={'disk_write_s':r'\PhysicalDisk(_Total)\Avg. Disk sec/Write','disk_queue':r'\PhysicalDisk(_Total)\Current Disk Queue Length','pages_in_s':r'\Memory\Pages Input/sec','frequency_percent':r'\Processor Information(_Total)\% of Maximum Frequency'}
pd.PdhAddEnglishCounterW.argtypes=[w.HANDLE,w.LPCWSTR,c.c_size_t,c.POINTER(w.HANDLE)]
pd.PdhCollectQueryData.argtypes=[w.HANDLE]
pd.PdhGetFormattedCounterValue.argtypes=[w.HANDLE,w.DWORD,c.c_void_p,c.POINTER(Value)]
for key,path in paths.items():
 handle=w.HANDLE();rc=pd.PdhAddEnglishCounterW(query,path,0,c.byref(handle))
 if rc==0:handles[key]=handle
 else:errors[key]=rc
def cpu():
 values=[w.FILETIME() for _ in range(3)];assert kernel.GetSystemTimes(*[c.byref(t) for t in values])
 return [t.dwHighDateTime*2**32+t.dwLowDateTime for t in values]
def sample(previous):
 current=cpu();delta=[a-b for a,b in zip(current,previous)];den=delta[1]+delta[2]
 m=Memory();m.length=c.sizeof(m);assert kernel.GlobalMemoryStatusEx(c.byref(m))
 row={'wall_ns':time.time_ns(),'cpu_percent':100*(den-delta[0])/den if den else None,'available_bytes':m.available,'total_bytes':m.total,'counter_status':{}}
 pd.PdhCollectQueryData(query)
 for key,handle in handles.items():
  value=Value();rc=pd.PdhGetFormattedCounterValue(handle,0x200,None,c.byref(value));row['counter_status'][key]=[rc,value.status]
  if rc==0 and value.status in [0,1]:row[key]=value.value
 return current,row
previous=cpu();pd.PdhCollectQueryData(query);rows=[]
for _ in range(10):time.sleep(1);previous,row=sample(previous);row['phase']='before';rows.append(row)
script=(p/(sys.argv[1] if len(sys.argv)>1 else 'response_candidate_compare.py')).read_bytes()
output=p/(sys.argv[2] if len(sys.argv)>2 else 'host-candidate-telemetry.json')
proc=subprocess.Popen(['C:/Windows/System32/OpenSSH/ssh.exe','-o','BatchMode=yes','-o','ConnectTimeout=10','zjh@192.168.29.128','python3','-'],stdin=subprocess.PIPE)
proc.stdin.write(script);proc.stdin.close()
while proc.poll() is None:
 time.sleep(1);previous,row=sample(previous);row['phase']='comparison';rows.append(row)
 output.write_text(json.dumps({'counter_errors':errors,'samples':rows},indent=2))
for _ in range(10):time.sleep(1);previous,row=sample(previous);row['phase']='after';rows.append(row)
output.write_text(json.dumps({'counter_errors':errors,'samples':rows},indent=2))
print('host samples',len(rows),'counter errors',errors,'remote exit',proc.returncode)
raise SystemExit(proc.returncode)
