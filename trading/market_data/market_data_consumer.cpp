#include "market_data_consumer.h"
namespace Trading {
MarketDataConsumer::MarketDataConsumer(Common::ClientId client,Exchange::MEMarketUpdateLFQueue* output,const std::string& iface,const std::string& snap_ip,int snap_port,const std::string& inc_ip,int inc_port,std::atomic<bool>* trusted,std::atomic<uint64_t>* generation)
  :output_(output),logger_("trading_market_data_consumer_"+std::to_string(client)+".log"),incremental_(logger_),snapshot_(logger_),
   trusted_(trusted?trusted:&local_trusted_),generation_(generation?generation:&local_generation_) {
  trusted_->store(false);observed_generation_=generation_->fetch_add(1)+1;
  ASSERT(incremental_.init(inc_ip,iface,inc_port,true)>=0 && incremental_.join(inc_ip),"cannot subscribe incremental stream");
  ASSERT(snapshot_.init(snap_ip,iface,snap_port,true)>=0 && snapshot_.join(snap_ip),"cannot subscribe snapshot stream");
  incremental_.recv_callback_=[this](auto* socket){receive(socket);};snapshot_.recv_callback_=incremental_.recv_callback_;
}
void MarketDataConsumer::stale() {
  trusted_->store(false,std::memory_order_release);observed_generation_=generation_->fetch_add(1,std::memory_order_acq_rel)+1;
  recovery_.restart();candidate_.reset();logger_.log("MARKET STALE: generation:% queue_full:%\n",observed_generation_,output_->full_count());
}
void MarketDataConsumer::run() {
  while(run_) {
    if(generation_->load(std::memory_order_acquire)!=observed_generation_) {
      trusted_->store(false);observed_generation_=generation_->load();recovery_.restart();candidate_.reset();
    }
    incremental_.sendAndRecv();snapshot_.sendAndRecv();publishRecovery();
  }
  logger_.log("RECOVERY COUNTERS gaps:% duplicates:% cache_overflow:% bad_hash:% recoveries:% queue_full:% high_watermark:%\n",recovery_.gaps_,recovery_.duplicates_,recovery_.overflows_,recovery_.hash_failures_,recovery_.recoveries_,output_->full_count(),output_->high_watermark());
}
void MarketDataConsumer::receive(Common::McastSocket* socket) {
  if(socket->receive_fault_ || socket->next_rcv_valid_index_%Common::Wire::MarketSize) {socket->next_rcv_valid_index_=0;stale();return;}
  for(size_t offset=0;offset<socket->next_rcv_valid_index_;offset+=Common::Wire::MarketSize) {
    Exchange::MDPMarketUpdate update;
    if(!Common::Wire::decode(reinterpret_cast<const uint8_t*>(socket->inbound_data_.data()+offset),update)) {stale();break;}
    process(socket==&snapshot_,update);
  }
  socket->next_rcv_valid_index_=0;
}
void MarketDataConsumer::process(bool snapshot,const Exchange::MDPMarketUpdate& update) {
  if(snapshot && !recovery_.recovering())return;
  if(!snapshot && !recovery_.recovering()) {
    if(recovery_.liveCandidate(update)) {
      if(output_->try_push(update.me_market_update_)) {recovery_.commitLive();return;}
      stale();
    } else if(!recovery_.recovering())return;
    else {trusted_->store(false);observed_generation_=generation_->fetch_add(1)+1;candidate_.reset();}
  }
  recovery_.ingest(snapshot,update);candidate_=recovery_.build();
  if(candidate_) {
    Exchange::MEMarketUpdate marker;marker.type_=Exchange::MarketUpdateType::RECOVERY_COMMIT;marker.order_id_=candidate_->watermark;marker.priority_=observed_generation_;marker.state_hash_=candidate_->hash;
    candidate_->events.push_back(marker);publishRecovery();
  }
}
void MarketDataConsumer::publishRecovery() {
  if(!candidate_)return;
  if(output_->try_push_batch(candidate_->events)) {
    logger_.log("RECOVERY PUBLISHED watermark:% hash:% generation:% events:%\n",candidate_->watermark,candidate_->hash,observed_generation_,candidate_->events.size());
    recovery_.commit(*candidate_);candidate_.reset();
  } else std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
}
