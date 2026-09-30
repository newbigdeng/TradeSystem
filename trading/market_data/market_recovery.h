#pragma once
#include <map>
#include <optional>
#include "common/book_hash.h"
namespace Trading {
class MarketRecovery {
 public:
  using Update=Exchange::MDPMarketUpdate;
  using Event=Exchange::MEMarketUpdate;
  using Type=Exchange::MarketUpdateType;
  struct Batch {std::vector<Event> events;uint64_t watermark,hash;};
  explicit MarketRecovery(size_t limit=Common::ME_MAX_MARKET_UPDATES):limit_(limit) {ASSERT(limit>=Common::ME_MAX_TICKERS+2,"recovery cache too small");}
  void restart() {recovering_=true;snapshot_.clear();incremental_.clear();invalid_cycle_=true;}
  bool recovering() const noexcept {return recovering_;}
  uint64_t expected() const noexcept {return expected_;}
  size_t cached() const noexcept {return snapshot_.size()+incremental_.size();}
  // Consumer checks output admission before committing this sequence.
  bool liveCandidate(const Update& update) {
    if(recovering_)return false;
    if(update.seq_num_==expected_)return true;
    if(update.seq_num_<expected_) {++duplicates_;return false;}
    ++gaps_;restart();return false;
  }
  void commitLive() {++expected_;}
  bool ingest(bool snapshot,const Update& update) {
    if(!recovering_)return false;
    if(snapshot) {
      if(!update.snapshot_cycle_ || update.snapshot_cycle_<cycle_)return false;
      if(update.snapshot_cycle_>cycle_) {
        cycle_=update.snapshot_cycle_;snapshot_.clear();invalid_cycle_=false;
        for(auto it=incremental_.begin();it!=incremental_.end() && it->first<=update.watermark_;)it=incremental_.erase(it);
      }
      if(invalid_cycle_)return false;
      auto found=snapshot_.find(update.seq_num_);
      if(found!=snapshot_.end()) {
        if(found->second.me_market_update_.toString()!=update.me_market_update_.toString() || found->second.watermark_!=update.watermark_ || found->second.state_hash_!=update.state_hash_) {invalid_cycle_=true;snapshot_.clear();++invalid_;}
        else ++duplicates_;
        return false;
      }
      snapshot_.emplace(update.seq_num_,update);
    } else {
      auto [found,inserted]=incremental_.emplace(update.seq_num_,update);
      if(!inserted) {
        ++duplicates_;
        if(found->second.me_market_update_.toString()!=update.me_market_update_.toString()) {restart();++invalid_;return false;}
      }
    }
    if(cached()>limit_) {snapshot_.clear();incremental_.clear();invalid_cycle_=true;++overflows_;return false;}
    return true;
  }
  std::optional<Batch> build() {
    if(snapshot_.empty() || invalid_cycle_)return std::nullopt;
    const auto& first=snapshot_.begin()->second;const auto& last=snapshot_.rbegin()->second;
    if(first.seq_num_!=0 || first.me_market_update_.type_!=Type::SNAPSHOT_START || last.me_market_update_.type_!=Type::SNAPSHOT_END)return std::nullopt;
    const auto watermark=first.watermark_;
    if(last.watermark_!=watermark || first.me_market_update_.order_id_!=watermark || last.me_market_update_.order_id_!=watermark || first.state_hash_!=last.state_hash_) {invalid_cycle_=true;++invalid_;return std::nullopt;}
    std::array<bool,Common::ME_MAX_TICKERS> cleared{};
    State state;Batch batch{{},watermark,0};uint64_t sequence=0;
    for(const auto& [id,wrapped]:snapshot_) {
      if(id!=sequence++ || wrapped.watermark_!=watermark || wrapped.snapshot_cycle_!=cycle_ || wrapped.state_hash_!=first.state_hash_)return std::nullopt;
      const auto& event=wrapped.me_market_update_;
      if(id==0)continue;
      if(event.type_==Type::SNAPSHOT_END) {if(id!=snapshot_.rbegin()->first)return std::nullopt;continue;}
      if(event.ticker_id_>=Common::ME_MAX_TICKERS)return std::nullopt;
      if(event.type_==Type::CLEAR) {
        if(cleared[event.ticker_id_])return std::nullopt;
        cleared[event.ticker_id_]=true;
      } else if(event.type_!=Type::ADD || !cleared[event.ticker_id_])return std::nullopt;
      if(!apply(state,event))return std::nullopt;
      batch.events.push_back(event);
    }
    if(!std::all_of(cleared.begin(),cleared.end(),[](bool value){return value;}))return std::nullopt;
    if(Common::bookHash(flatten(state))!=first.state_hash_) {++hash_failures_;invalid_cycle_=true;return std::nullopt;}
    uint64_t next=watermark+1;
    for(const auto& [id,wrapped]:incremental_) {
      if(id<next)continue;
      if(id!=next)return std::nullopt;
      if(!apply(state,wrapped.me_market_update_))return std::nullopt;
      batch.events.push_back(wrapped.me_market_update_);++next;
    }
    batch.watermark=next-1;batch.hash=Common::bookHash(flatten(state));return batch;
  }
  void commit(const Batch& batch) {expected_=batch.watermark+1;recovering_=false;snapshot_.clear();incremental_.clear();++recoveries_;}
  uint64_t gaps_=0,duplicates_=0,overflows_=0,invalid_=0,hash_failures_=0,recoveries_=0;
 private:
  using State=std::array<std::map<Common::OrderId,Event>,Common::ME_MAX_TICKERS>;
  static std::vector<Event> flatten(const State& state) {
    std::vector<Event> events;for(const auto& book:state)for(const auto& [id,event]:book){(void)id;events.push_back(event);}return events;
  }
  static bool apply(State& state,const Event& event) {
    if(event.ticker_id_>=Common::ME_MAX_TICKERS)return false;
    auto &orders=state[event.ticker_id_];
    if(event.type_==Type::CLEAR){orders.clear();return true;}
    if(event.type_==Type::TRADE)return event.qty_>0 && event.qty_!=Common::Qty_INVALID && event.price_>0 && event.price_!=Common::Price_INVALID && (event.side_==Common::Side::BUY || event.side_==Common::Side::SELL);
    if(!event.order_id_ || event.order_id_>=Common::ME_MAX_ORDER_IDS)return false;
    auto found=orders.find(event.order_id_);
    if(event.type_==Type::ADD) {
      if(found!=orders.end() || event.price_<=0 || event.price_==Common::Price_INVALID || !event.qty_ || event.qty_==Common::Qty_INVALID || !event.priority_ || event.priority_==Common::Priority_INVALID || (event.side_!=Common::Side::BUY && event.side_!=Common::Side::SELL))return false;
      orders.emplace(event.order_id_,event);return true;
    }
    if(found==orders.end() || found->second.side_!=event.side_ || found->second.price_!=event.price_)return false;
    if(event.type_==Type::CANCEL){orders.erase(found);return true;}
    if(event.type_==Type::MODIFY && event.qty_>0 && event.qty_!=Common::Qty_INVALID) {found->second.qty_=event.qty_;return true;}
    return false;
  }
  size_t limit_;bool recovering_=true,invalid_cycle_=false;
  uint64_t cycle_=0,expected_=1;
  std::map<uint64_t,Update> snapshot_,incremental_;
};
}
