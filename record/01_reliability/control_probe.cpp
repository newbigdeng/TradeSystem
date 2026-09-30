#include <chrono>
#include <cmath>
#include <memory>
#include <stdexcept>
#include "strategy/trade_engine.h"
void check_control(bool v,const char *m){if(!v)throw std::runtime_error(m);}
double feature(Trading::FeatureEngine& engine,Common::TickerId ticker) {
#if defined(TRADE_CONTROL_BASELINE) || defined(TRADE_FEATURE_BASELINE)
  (void)ticker;return engine.getMktPrice();
#else
  return engine.getMktPrice(ticker);
#endif
}
void feature_case(bool benchmark) {
  Common::Logger logger("feature_probe.log");Trading::FeatureEngine engine(&logger);
  auto first=std::make_unique<Trading::MarketOrderBook>(0,&logger),second=std::make_unique<Trading::MarketOrderBook>(1,&logger);
  *const_cast<Trading::BBO*>(first->getBBO())={100,102,5,5};
  *const_cast<Trading::BBO*>(second->getBBO())={200,202,5,5};
  const auto begin=std::chrono::steady_clock::now();constexpr uint64_t count=20000;double checksum=0;
  for(uint64_t i=0;i<(benchmark?count:1);++i) {
    engine.onOrderBookUpdate(0,100,Common::Side::BUY,first.get());
    engine.onOrderBookUpdate(1,200,Common::Side::BUY,second.get());
    checksum+=feature(engine,1);
  }
  const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();
  if(benchmark) {
    check_control(checksum==201*count,"feature benchmark checksum differs");
    std::cout<<"{\"mode\":\"feature\",\"operations\":"<<count*2<<",\"elapsed_ns\":"<<ns<<",\"ns_per_operation\":"<<double(ns)/(count*2)<<",\"checksum\":"<<checksum<<"}\n";return;
  }
  check_control(feature(engine,0)==101 && feature(engine,1)==201,"features leaked across tickers");
  *const_cast<Trading::BBO*>(first->getBBO())={};engine.onOrderBookUpdate(0,0,Common::Side::BUY,first.get());
  check_control(!std::isfinite(feature(engine,0)),"invalid quote retained a stale feature");
  *const_cast<Trading::BBO*>(first->getBBO())={100,102,0,0};engine.onOrderBookUpdate(0,0,Common::Side::BUY,first.get());
  check_control(!std::isfinite(feature(engine,0)),"zero depth accepted");
}
void state_case() {
  Common::Logger logger("state_probe.log");Trading::PositionKeeper positions(&logger);Common::TradeEngineCfgHashMap cfg{};
  Trading::RiskManager risk(&logger,&positions,cfg);Trading::OrderManager manager(&logger,nullptr,risk);
  auto *order=const_cast<Trading::OMOrder*>(&manager.getOMOrderSideHashMap(0)->at(Common::sideToIndex(Common::Side::BUY)));
  *order={0,7,Common::Side::BUY,100,5,Trading::OMOrderState::PENDING_CANCEL};
  Exchange::MEClientResponse response{Exchange::ClientResponseType::CANCEL_REJECTED,1,0,7,9,Common::Side::INVALID,100,0,5};
  manager.onOrderUpdate(&response);
  check_control(order->order_state_==Trading::OMOrderState::LIVE,"cancel rejection left order pending");
  response={Exchange::ClientResponseType::FILLED,1,0,7,9,Common::Side::BUY,100,5,0};manager.onOrderUpdate(&response);
  response={Exchange::ClientResponseType::ACCEPTED,1,0,7,9,Common::Side::BUY,100,0,5};manager.onOrderUpdate(&response);
  check_control(order->order_state_==Trading::OMOrderState::DEAD,"late acceptance revived a filled order");
}
void admission_case(bool benchmark) {
  Exchange::ClientRequestLFQueue requests(1);Exchange::ClientResponseLFQueue responses(64);Exchange::MEMarketUpdateLFQueue market(64);
  Common::TradeEngineCfgHashMap cfg{};for(auto &c:cfg)c={1,0,{10,10,-1000000}};
  Trading::TradeEngine engine(1,Common::AlgoType::RANDOM,cfg,&requests,&responses,&market);
#ifndef TRADE_CONTROL_BASELINE
  engine.setMarketTrusted(true);
#endif
  if(!benchmark) {
    Exchange::MEClientRequest too_large{Exchange::ClientRequestType::NEW,1,0,1,Common::Side::BUY,100,11};
    engine.sendClientRequest(&too_large);check_control(requests.size()==0,"RANDOM bypassed the final risk gate");
#ifndef TRADE_CONTROL_BASELINE
    auto request=too_large;request.qty_=6;request.order_id_=2;
    check_control(engine.sendClientRequest(&request),"valid initial request rejected");Exchange::MEClientRequest ignored;requests.try_pop(ignored);
    request.order_id_=3;check_control(!engine.sendClientRequest(&request),"pending orders omitted from worst-case position");
    Exchange::MEClientResponse fill{Exchange::ClientResponseType::FILLED,1,0,2,1,Common::Side::BUY,100,2,4};fill.response_id_=1;
    engine.onOrderUpdate(&fill);engine.onOrderUpdate(&fill);
    check_control(engine.positionInfo(0)->position_==2,"duplicate execution changed position twice");
    Exchange::MEClientResponse canceled{Exchange::ClientResponseType::CANCELED,1,0,2,1,Common::Side::BUY,100,0,4};canceled.response_id_=2;engine.onOrderUpdate(&canceled);
    request.qty_=8;check_control(engine.sendClientRequest(&request),"canceled reservation was not released");
    requests.try_pop(ignored);engine.setMarketTrusted(false);request.order_id_=4;request.qty_=1;
    check_control(!engine.sendClientRequest(&request),"stale market admitted a new decision");
    Common::Logger logger("ambiguous_response.log");Trading::PositionKeeper positions(&logger);Trading::RiskManager risk(&logger,&positions,cfg);
    request.qty_=6;request.order_id_=8;
    check_control(risk.reserve(request)==Trading::RiskCheckResult::ALLOWED,"initial ambiguous-response reservation failed");
    Exchange::MEClientResponse ambiguous{Exchange::ClientResponseType::REJECTED,1,0,8,1,Common::Side::BUY,100,0,0,Exchange::RejectReason::DUPLICATE_ID};
    check_control(!risk.onResponse(ambiguous)&&risk.pending(0,Common::Side::BUY)==6,"duplicate rejection freed the original order");
    ambiguous.type_=Exchange::ClientResponseType::CANCEL_REJECTED;
    check_control(!risk.onResponse(ambiguous)&&risk.pending(0,Common::Side::BUY)==6,"ambiguous cancel rejection freed risk");
    ambiguous.leaves_qty_=6;check_control(risk.onResponse(ambiguous),"matching cancel rejection was not recoverable");
    ambiguous.type_=Exchange::ClientResponseType::CANCELED;
    check_control(risk.onResponse(ambiguous)&&risk.pending(0,Common::Side::BUY)==0,"confirmed cancel did not release risk");
#endif
    return;
  }
  constexpr uint64_t count=10000;uint64_t checksum=0;const auto begin=std::chrono::steady_clock::now();
  for(uint64_t i=0;i<count;++i) {
    Exchange::MEClientRequest request{Exchange::ClientRequestType::NEW,1,0,i+1,Common::Side::BUY,100,1};
#ifdef TRADE_CONTROL_BASELINE
    engine.sendClientRequest(&request);
#else
    check_control(engine.sendClientRequest(&request),"valid benchmark request rejected");
#endif
    Exchange::MEClientRequest actual;check_control(requests.try_pop(actual)&&actual.order_id_==i+1,"admission queue differs");++checksum;
    Exchange::MEClientResponse ack{Exchange::ClientResponseType::ACCEPTED,1,0,i+1,i+1,Common::Side::BUY,100,0,1};
#ifndef TRADE_CONTROL_BASELINE
    ack.response_id_=i*2+1;
#endif
    engine.onOrderUpdate(&ack);ack.type_=Exchange::ClientResponseType::CANCELED;
#ifndef TRADE_CONTROL_BASELINE
    ++ack.response_id_;
#endif
    engine.onOrderUpdate(&ack);
  }
  const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();
  std::cout<<"{\"mode\":\"admission\",\"operations\":"<<count<<",\"elapsed_ns\":"<<ns<<",\"ns_per_operation\":"<<double(ns)/count<<",\"checksum\":"<<checksum<<"}\n";
}
void lifecycle_case() {
  const auto begin=std::chrono::steady_clock::now();std::atomic<int> value{0};
  auto *worker=Common::createAndStartThread(-1,"temporary-name",[&]{value.store(42);});worker->join();delete worker;
  check_control(value.load()==42,"thread task did not finish");
  const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();
  std::cout<<"{\"mode\":\"thread_start_join\",\"operations\":1,\"elapsed_ns\":"<<ns<<",\"ns_per_operation\":"<<ns<<",\"checksum\":42}\n";
}
#ifndef TRADE_CONTROL_BASELINE
void concurrent_case() {
  Exchange::ClientRequestLFQueue requests(3);Exchange::ClientResponseLFQueue responses(3);Exchange::MEMarketUpdateLFQueue market(3);
  Common::TradeEngineCfgHashMap cfg{};for(auto& c:cfg)c={1,0,{1,1000000,-1000000}};
  Trading::TradeEngine engine(1,Common::AlgoType::RANDOM,cfg,&requests,&responses,&market);engine.setMarketTrusted(true);
  std::atomic<uint64_t> accepted{0};std::atomic<int> complete{0};uint64_t received=0;
  auto producer=[&](uint64_t base) {
    for(uint64_t i=1;i<=10000;++i) {
      Exchange::MEClientRequest request{Exchange::ClientRequestType::NEW,1,0,base+i,Common::Side::BUY,100,1};
      if(engine.sendClientRequest(&request))accepted.fetch_add(1);
    }
    complete.fetch_add(1);
  };
  std::thread first(producer,0),second(producer,10000);Exchange::MEClientRequest message;
  while(complete.load()!=2 || requests.peek()) {if(requests.try_pop(message))++received;else std::this_thread::yield();}
  first.join();second.join();check_control(received==accepted.load(),"concurrent admission lost requests");
}
#else
void concurrent_case() {throw std::runtime_error("concurrent admission test requires the repaired producer serialization");}
#endif
int main(int argc,char** argv) {
  try {
    if(argc!=2)return 2;
    const std::string mode=argv[1];
    if(mode=="state")state_case();else if(mode=="admission")admission_case(false);else if(mode=="admission_benchmark")admission_case(true);
    else if(mode=="feature")feature_case(false);else if(mode=="feature_benchmark")feature_case(true);else if(mode=="lifecycle")lifecycle_case();else if(mode=="concurrent")concurrent_case();else return 2;
    std::cout<<"PASS "<<mode<<'\n';return 0;
  }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
