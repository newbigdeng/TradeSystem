#include <atomic>
#include <iostream>
#include <thread>
#include "common/time_utils.h"
#include "common/perf_utils.h"
struct Probe {
  std::atomic<uint64_t> calls{0};
  template<class... T> void log(const char*,const T&...) { ++calls; }
};
Probe probe;
std::atomic<uint64_t> evaluated{0};
Probe& evaluatedLogger() {++evaluated;return probe;}
void exercise() {
  for(int i=0;i<1024;++i) {
    START_MEASURE(test_duration);
    END_MEASURE(test_duration, evaluatedLogger());
    TTT_MEASURE(test_event, evaluatedLogger());
  }
}
int main() {
  std::thread a(exercise),b(exercise);a.join();b.join();
  const auto expected=TRADE_PERF_TRACE_EVERY==0 ? 0 : (1024/TRADE_PERF_TRACE_EVERY)*4;
  if(probe.calls!=expected || evaluated!=expected) return 1;
  std::cout<<"PASS trace_every="<<TRADE_PERF_TRACE_EVERY<<" records="<<probe.calls<<" logger_evaluations="<<evaluated<<'\n';
}
