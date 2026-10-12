#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Measure {
using Clock=std::chrono::steady_clock;
inline uint64_t now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
inline void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
inline uint64_t quantile(const std::vector<uint64_t>& sorted,size_t numerator) {
  return sorted[(numerator*sorted.size()+99)/100-1];
}
inline void output(const std::string& name,const std::vector<uint64_t>& samples,
                   uint64_t elapsed,uint64_t checksum,uint64_t errors=0) {
  require(!samples.empty(),"no timed operations");
  auto sorted=samples;std::sort(sorted.begin(),sorted.end());
  std::ofstream raw(name+".csv");raw<<"operation,elapsed_ns\n";
  for(size_t i=0;i<samples.size();++i)raw<<i<<','<<samples[i]<<'\n';
  raw.close();require(bool(raw),"sample write failed");
  std::cout<<"{\"name\":\""<<name<<"\",\"operations\":"<<samples.size()
           <<",\"elapsed_ns\":"<<elapsed<<",\"completed_per_s\":"<<samples.size()*1e9/elapsed
           <<",\"p50_ns\":"<<quantile(sorted,50)<<",\"p95_ns\":"<<quantile(sorted,95)
           <<",\"p99_ns\":"<<quantile(sorted,99)<<",\"max_ns\":"<<sorted.back()
           <<",\"checksum\":"<<checksum<<",\"errors\":"<<errors<<"}\n";
}
inline void clockOverhead() {
  std::vector<uint64_t> samples(10000);
  const auto window=now();
  for(auto& ns:samples){auto start=now();ns=now()-start;}
  output("clock_empty",samples,now()-window,0);
}
}
