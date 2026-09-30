#include <iostream>
#include "common/time_utils.h"
int main() {
  constexpr uint64_t count=100000;uint64_t checksum=0;std::string stamp;
  const auto start=std::chrono::steady_clock::now();
  for(uint64_t i=0;i<count;++i)checksum+=Common::getCurrentTimeStr(&stamp).size();
  const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();
  if(checksum!=count*18)return 1;
  std::cout<<"{\"operations\":"<<count<<",\"elapsed_ns\":"<<ns<<",\"ns_per_operation\":"<<double(ns)/count<<",\"checksum\":"<<checksum<<"}\n";
}
