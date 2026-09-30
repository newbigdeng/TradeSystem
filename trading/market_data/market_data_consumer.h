#pragma once
#include "common/mcast_socket.h"
#include "common/market_protocol.h"
#include "market_recovery.h"
namespace Trading {
class MarketDataConsumer {
 public:
  MarketDataConsumer(Common::ClientId,Exchange::MEMarketUpdateLFQueue*,const std::string&,const std::string&,int,const std::string&,int,
                     std::atomic<bool>* trusted=nullptr,std::atomic<uint64_t>* generation=nullptr);
  ~MarketDataConsumer(){stop();}
  void start(){run_=true;worker_.reset(Common::createAndStartThread(-1,"Trading/MarketDataConsumer",[this]{run();}));}
  void stop(){run_=false;if(worker_&&worker_->joinable())worker_->join();}
  MarketDataConsumer(const MarketDataConsumer&)=delete;MarketDataConsumer& operator=(const MarketDataConsumer&)=delete;
 private:
  void run();void receive(Common::McastSocket*);void stale();void process(bool,const Exchange::MDPMarketUpdate&);void publishRecovery();
  Exchange::MEMarketUpdateLFQueue* output_;
  Common::Logger logger_;
  Common::McastSocket incremental_,snapshot_;
  MarketRecovery recovery_;
  std::optional<MarketRecovery::Batch> candidate_;
  std::atomic<bool> local_trusted_{false},run_{false};
  std::atomic<uint64_t> local_generation_{0};
  std::atomic<bool>* trusted_;std::atomic<uint64_t>* generation_;uint64_t observed_generation_=0;
  std::unique_ptr<std::thread> worker_;
};
}
