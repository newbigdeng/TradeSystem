import pathlib, subprocess, tarfile, io, os, sys, json
r=pathlib.Path('/home/zjh/XiangMu/TradeSystem-rebuild')
e=pathlib.Path('/home/zjh/trade-measurements/08_audit_decomposition')
e.mkdir(exist_ok=False)
for label in ['baseline','flush']:
 s=e/(label+'-source');s.mkdir()
 if label=='baseline':
  with tarfile.open('/home/zjh/trade-measurements/07_e2e_latency/before.tar.gz') as t:t.extractall(s,filter='data')
 else:
  data=subprocess.check_output(['git','archive','HEAD'],cwd=r)
  with tarfile.open(fileobj=io.BytesIO(data)) as t:t.extractall(s,filter='data')
 p=s/'common/critical_journal.h';text=p.read_text()
 text=text.replace('#include <cerrno>', '#include <cerrno>\n#include <array>\n#include <vector>\n#include <cstdio>\n#include <ctime>')
 text=text.replace('explicit CriticalJournal(const std::string& path) {','explicit CriticalJournal(const std::string& path) {\n    trace_.reserve(8192);')
 text=text.replace('~CriticalJournal(){if(fd_>=0)::close(fd_);}', '''~CriticalJournal(){
    if(auto* out=std::fopen("audit_trace.csv","w")) {
      std::fprintf(out,"start,locked,written,synced,cpu_start,cpu_end,text\\n");
      for(const auto& t:trace_) std::fprintf(out,"%lld,%lld,%lld,%lld,%lld,%lld,%s\\n",t.start,t.locked,t.written,t.synced,t.cpu_start,t.cpu_end,t.text.data());
      std::fclose(out);
    }
    if(fd_>=0)::close(fd_);
  }''')
 text=text.replace('std::lock_guard<std::mutex> lock(writer_);const auto record=', 'Trace trace{};trace.cpu_start=now(CLOCK_THREAD_CPUTIME_ID);trace.start=now(CLOCK_MONOTONIC);\n    std::lock_guard<std::mutex> lock(writer_);trace.locked=now(CLOCK_MONOTONIC);const auto record=')
 text=text.replace('int rc;do {rc=::fsync(fd_);}', 'trace.written=now(CLOCK_MONOTONIC);\n    int rc;do {rc=::fsync(fd_);}')
 text=text.replace('ASSERT(rc==0,"critical journal fsync failed");++records_;', '''trace.synced=now(CLOCK_MONOTONIC);trace.cpu_end=now(CLOCK_THREAD_CPUTIME_ID);
    ASSERT(rc==0,"critical journal fsync failed");++records_;
    std::snprintf(trace.text.data(),trace.text.size(),"%s",text.c_str());
    ASSERT(trace_.size()<trace_.capacity(),"diagnostic trace capacity exhausted");trace_.push_back(trace);''')
 text=text.replace('int fd_=-1;', '''struct Trace { long long start,locked,written,synced,cpu_start,cpu_end;std::array<char,160> text; };
  std::vector<Trace> trace_;
  static long long now(clockid_t id) {timespec t{};::clock_gettime(id,&t);return t.tv_sec*1000000000LL+t.tv_nsec;}
  int fd_=-1;''')
 p.write_text(text)
 (e/(label+'-instrumentation.patch')).write_text(subprocess.check_output(['diff','-u',str(r/'common/critical_journal.h'),str(p)],text=True) if False else text)
 b=e/(label+'-build')
 with (e/(label+'-build.log')).open('w') as out:
  subprocess.run(['cmake','-S',str(s),'-B',str(b),'-DCMAKE_BUILD_TYPE=Release','-DBUILD_TESTING=OFF','-DTRADE_PERF_TRACE_EVERY=1'],stdout=out,stderr=subprocess.STDOUT,check=True)
  subprocess.run(['cmake','--build',str(b),'--target','exchange_main','-j2'],stdout=out,stderr=subprocess.STDOUT,check=True)
 print('built',label,flush=True)
sys.path.insert(0,str(r/'record/02_reproducible_measurement'));import load_generator as g
results=[]
for round in range(3):
 for label in (['baseline','flush'] if round%2==0 else ['flush','baseline']):
  cfg={'clients':4,'tickers':8,'seed':20260928,'requests':256,'mode':'open','rate':50,'timeout_s':20,'pause_ms':0}
  d=g.run(e/(label+'-build/exchange_main'),e/f'{round}-{label}',cfg,[0,1,2,3],[4])
  results.append({'round':round,'label':label,'result':d})
  (e/'results.json').write_text(json.dumps(results,indent=2))
  print(round,label,d['correctness_ok'],d.get('statistics',{}).get('actual_send_to_response'),flush=True)
