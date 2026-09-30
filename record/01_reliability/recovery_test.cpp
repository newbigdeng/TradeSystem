#include <chrono>
#include <random>
#include <stdexcept>
#include "trading/market_data/market_recovery.h"
#include "common/market_protocol.h"
using U=Exchange::MDPMarketUpdate;using E=Exchange::MEMarketUpdate;using T=Exchange::MarketUpdateType;
void require_recovery(bool value,const char* message){if(!value)throw std::runtime_error(message);}
std::vector<U> cycle(uint64_t identity,uint64_t watermark=42) {
  std::vector<E> orders;
  for(uint64_t id=1;id<=64;++id)orders.push_back({T::ADD,id,Common::TickerId(id%8),Common::Side::BUY,100,1,id});
  const auto hash=Common::bookHash(orders);std::vector<U> frames;
  auto push=[&](E e){frames.push_back({frames.size(),e,identity,watermark,hash});};
  push({T::SNAPSHOT_START,watermark});
  for(Common::TickerId ticker=0;ticker<8;++ticker)push({T::CLEAR,Common::OrderId_INVALID,ticker});
  for(const auto& order:orders)push(order);
  push({T::SNAPSHOT_END,watermark});return frames;
}
void inject(Trading::MarketRecovery& r,const std::vector<U>& frames){for(const auto& frame:frames)r.ingest(true,frame);}
void test() {
  auto frames=cycle(1);
  Trading::MarketRecovery good;std::mt19937 rng(991);auto shuffled=frames;std::shuffle(shuffled.begin(),shuffled.end(),rng);
  inject(good,shuffled);good.ingest(true,frames[3]);auto batch=good.build();
  require_recovery(batch&&batch->events.size()==72&&batch->watermark==42,"complete reordered/duplicated snapshot rejected");
  require_recovery(good.duplicates_==1,"snapshot duplicate not counted");
  good.commit(*batch);require_recovery(!good.recovering()&&good.expected()==43,"recovery commit failed");
  U duplicate{42,{T::TRADE,Common::OrderId_INVALID,0,Common::Side::BUY,100,1}};
  require_recovery(!good.liveCandidate(duplicate)&&!good.recovering(),"old incremental triggered false gap");
  duplicate.seq_num_=44;require_recovery(!good.liveCandidate(duplicate)&&good.recovering(),"incremental gap kept market live");
  for(size_t missing:{size_t(0),size_t(5),frames.size()-1}) {
    Trading::MarketRecovery r;auto incomplete=frames;incomplete.erase(incomplete.begin()+missing);
    for(size_t i=0;i<incomplete.size();++i)incomplete[i].seq_num_=i;
    inject(r,incomplete);require_recovery(!r.build(),"snapshot missing START/CLEAR/END accepted");
  }
  Trading::MarketRecovery mixed;
  auto newer=cycle(2);for(size_t i=0;i<frames.size();++i)mixed.ingest(true,i%2?newer[i]:frames[i]);
  require_recovery(!mixed.build(),"interleaved snapshot cycles mixed");
  Trading::MarketRecovery corrupt;auto broken=frames;broken.back().state_hash_^=1;inject(corrupt,broken);require_recovery(!corrupt.build(),"bad hash accepted");
  inject(corrupt,cycle(2));require_recovery(bool(corrupt.build()),"new complete cycle did not repair previous bad hash");
  Trading::MarketRecovery gap;
  U first{43,{T::ADD,65,0,Common::Side::BUY,100,1,65}},second{44,{T::MODIFY,65,0,Common::Side::BUY,100,2,65}};
  gap.ingest(false,second);inject(gap,frames);require_recovery(!gap.build(),"gap after snapshot watermark accepted");
  gap.ingest(false,first);gap.ingest(false,first);batch=gap.build();require_recovery(batch&&batch->watermark==44&&batch->events.size()==74,"late missing incremental did not repair gap");
  std::vector<E> expected;for(const auto& frame:frames)if(frame.me_market_update_.type_==T::ADD)expected.push_back(frame.me_market_update_);
  auto extra=first.me_market_update_;extra.qty_=2;expected.push_back(extra);
  require_recovery(batch->hash==Common::bookHash(expected),"snapshot plus continuous incrementals rebuilt a different state");
  Trading::MarketRecovery bounded(10);inject(bounded,frames);require_recovery(bounded.overflows_>0&&bounded.cached()<=10&&!bounded.build(),"cache overflow was unbounded or marked live");
  Common::LFQueue<E> output(74);require_recovery(output.try_push(first.me_market_update_)&&output.try_push(second.me_market_update_),"prefill failed");
  batch->events.push_back({T::RECOVERY_COMMIT});
  require_recovery(!output.try_push_batch(batch->events)&&output.size()==2,"full output published a partial recovery");
  E event;output.try_pop(event);output.try_pop(event);require_recovery(!output.try_push_batch(batch->events),"oversized batch accepted");
  batch->events.pop_back();require_recovery(output.try_push_batch(batch->events)&&output.size()==74,"exact-capacity batch failed");
  Common::LFQueue<int> wrapped(3);wrapped.try_push(1);wrapped.try_push(2);int n;wrapped.try_pop(n);
  require_recovery(wrapped.try_push_batch({3,4}),"wrapped batch failed");for(int want:{2,3,4})require_recovery(wrapped.try_pop(n)&&n==want,"wrapped batch reordered");
  const auto bytes=Common::Wire::encode(frames.back());U decoded;
  require_recovery(Common::Wire::decode(bytes.data(),decoded)&&decoded.snapshot_cycle_==1&&decoded.watermark_==42&&decoded.state_hash_==frames.back().state_hash_,"market metadata wire roundtrip failed");
}
int main(int argc,char** argv) {
  try {
    if(argc>1&&std::string(argv[1])=="benchmark") {
      Trading::MarketRecovery recovery;auto frames=cycle(1);constexpr uint64_t count=1000;uint64_t checksum=0;
      const auto begin=std::chrono::steady_clock::now();
      for(uint64_t round=0;round<count;++round) {
        recovery.restart();for(auto& frame:frames)frame.snapshot_cycle_=round+1;
        inject(recovery,frames);const auto batch=recovery.build();require_recovery(bool(batch),"valid recovery benchmark failed");
        checksum+=batch->events.size();recovery.commit(*batch);
      }
      const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();
      require_recovery(checksum==count*72,"recovery benchmark count differs");
      std::cout<<"{\"mode\":\"recovery\",\"operations\":"<<count<<",\"elapsed_ns\":"<<ns<<",\"ns_per_operation\":"<<double(ns)/count<<",\"checksum\":"<<checksum<<"}\n";
    } else {test();std::cout<<"PASS recovery fault matrix, bounded cache, state hash and atomic output admission\n";}
    return 0;
  }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
