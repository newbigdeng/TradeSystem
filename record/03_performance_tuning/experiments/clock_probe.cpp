#include <iostream>
#include "common/stage_trace.h"
#include "record/02_reproducible_measurement/benchmark_support.h"
int main(int argc,char**) {
  if(argc>1){std::cout<<StageTrace::now()<<'\n';return 0;}
  std::vector<uint64_t> clocks(10000),enabled(10000),disabled(10000);uint64_t max_span=0;
  for(auto& ns:clocks) {const auto a=StageTrace::now();const auto s=Measure::now();const auto b=StageTrace::now();Measure::require(a<=s&&s<=b,"steady_clock not bracketed by CLOCK_MONOTONIC");max_span=std::max(max_span,b-a);ns=b-a;}
  const auto empty_start=Measure::now();
  for(auto& ns:disabled){const auto a=Measure::now();ns=Measure::now()-a;}
  const auto empty_elapsed=Measure::now()-empty_start;
  const StageTrace::Key key{80,0,1,1};StageTrace::event(key,0);
  const auto window=Measure::now();
  for(auto& ns:enabled){const auto a=Measure::now();StageTrace::event(key,0);ns=Measure::now()-a;}
  const auto elapsed=Measure::now()-window;
  Measure::output("stage_record_call",enabled,elapsed,0);Measure::output("stage_empty_clock_pair",disabled,empty_elapsed,0);
  std::cout<<"{\"monotonic_steady_brackets\":10000,\"max_bracket_span_ns\":"<<max_span<<"}\n";
}
