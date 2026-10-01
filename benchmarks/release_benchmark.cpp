#include "common/mem_pool.h"
#include "common/opt_mem_pool.h"
#include "exchange/market_data/market_update.h"
#include "record/02_reproducible_measurement/benchmark_support.h"
#include <array>

template<class Pool> void benchmark(const std::string& name) {
  Pool pool(512);std::array<Exchange::MDPMarketUpdate*,256> objects{};
  constexpr size_t rounds=1000,warmup=100;
  std::vector<uint64_t> allocate(rounds*objects.size()),release(allocate.size());
  uint64_t checksum=0,index=0,window=0;
  for(size_t round=0;round<warmup+rounds;++round) {
    if(round==warmup)window=Measure::now();
    for(auto& object:objects) {
      const auto start=Measure::now();object=pool.allocate();const auto ns=Measure::now()-start;
      Measure::require(object!=nullptr,"pool allocation failed");object->seq_num_=round;
      if(round>=warmup)allocate[index++]=ns;
    }
    if(round>=warmup)index-=objects.size();
    for(auto* object:objects) {
      checksum+=object->seq_num_;
      const auto start=Measure::now();pool.deallocate(object);const auto ns=Measure::now()-start;
      if(round>=warmup)release[index++]=ns;
    }
    Measure::require(pool.available()==512,"pool leak");
  }
  const auto elapsed=Measure::now()-window;
  Measure::require(index==rounds*256 && checksum==uint64_t(warmup+rounds-1)*(warmup+rounds)*128,"pool checksum differs");
  Measure::output(name+"_allocate",allocate,elapsed,checksum);
  Measure::output(name+"_release",release,elapsed,checksum);
}
int main(int argc,char**) {
  try {
    Measure::clockOverhead();
    std::cout<<"NOTE OptMemPool is an alias of MemPool; labels are repeat controls. Per-call nanoseconds include clock overhead; rate is calls of this type per full loop second.\n";
    if(argc>1) {
      benchmark<OptCommon::OptMemPool<Exchange::MDPMarketUpdate>>("pool_control_b");
      benchmark<Common::MemPool<Exchange::MDPMarketUpdate>>("pool_control_a");
    } else {
      benchmark<Common::MemPool<Exchange::MDPMarketUpdate>>("pool_control_a");
      benchmark<OptCommon::OptMemPool<Exchange::MDPMarketUpdate>>("pool_control_b");
    }
    return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
