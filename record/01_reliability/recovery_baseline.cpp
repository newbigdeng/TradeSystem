// Preserved pre-refactor probe. Run only on the source commit in its JSON record.
// Private access is limited to this diagnostic executable, never production.
#include <bits/stdc++.h>
#include "common/mcast_socket.h"
#include "exchange/market_data/market_update.h"
#define private public
#include "trading/market_data/market_data_consumer.h"
#undef private
int main(int argc,char** argv) {
  Exchange::MEMarketUpdateLFQueue queue(128);
  auto *consumer=new Trading::MarketDataConsumer(90,&queue,"lo","233.252.14.1",0,"233.252.14.3",0);
  const bool benchmark=argc>1&&std::string(argv[1])=="benchmark";
  const uint64_t count=benchmark?1000:1;uint64_t checksum=0;
  const auto begin=std::chrono::steady_clock::now();
  for(uint64_t round=0;round<count;++round) {
    consumer->in_recovery_=true;consumer->snapshot_queued_msgs_.clear();consumer->incremental_queued_msgs_.clear();
    auto &messages=consumer->snapshot_queued_msgs_;
    messages[0]={Exchange::MarketUpdateType::SNAPSHOT_START,42};size_t sequence=1;
    if(benchmark) {
      for(Common::TickerId ticker=0;ticker<Common::ME_MAX_TICKERS;++ticker)messages[sequence++]={Exchange::MarketUpdateType::CLEAR,Common::OrderId_INVALID,ticker};
      for(uint64_t id=1;id<=64;++id)messages[sequence++]={Exchange::MarketUpdateType::ADD,id,Common::TickerId(id%8),Common::Side::BUY,100,1,id};
    }
    messages[sequence]={Exchange::MarketUpdateType::SNAPSHOT_END,42};
    consumer->checkSnapshotSync();
    if(!benchmark && !consumer->in_recovery_) {std::cerr<<"FAIL snapshot without any CLEAR was accepted as complete\n";std::cerr.flush();std::_Exit(1);}
    Exchange::MEMarketUpdate update;while(queue.try_pop(update))++checksum;
  }
  const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();
  if(benchmark) {
    if(checksum!=count*72)return 1;
    std::cout<<"{\"mode\":\"recovery\",\"operations\":"<<count<<",\"elapsed_ns\":"<<ns<<",\"ns_per_operation\":"<<double(ns)/count<<",\"checksum\":"<<checksum<<"}\n";
  }
  std::cout.flush();std::_Exit(0); // Old consumer destructor uses a fixed five-second sleep instead of a join.
}
