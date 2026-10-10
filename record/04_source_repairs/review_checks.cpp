#include <atomic>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include "common/lf_queue.h"
#ifndef OWNERSHIP_BASELINE
#include "trading/strategy/position_keeper.h"
#include "trading/strategy/market_order_book.h"
#endif

void require(bool ok,const char* reason) {if(!ok)throw std::runtime_error(reason);}

void ownership() {
  Common::LFQueue<uint64_t> queue(1);
#ifdef OWNERSHIP_BASELINE
  *queue.getNextToWriteTo()=111;queue.updateWriteIndex();
#else
  require(queue.try_push(111),"initial push");
#endif
  std::atomic<unsigned> phase{0};uint64_t forwarded=0;
  std::thread producer([&] {
    while(phase.load(std::memory_order_acquire)!=1)std::this_thread::yield();
#ifdef OWNERSHIP_BASELINE
    *queue.getNextToWriteTo()=222;queue.updateWriteIndex();
#else
    require(queue.try_push(222),"reuse released slot");
#endif
    phase.store(2,std::memory_order_release);
  });
#ifdef OWNERSHIP_BASELINE
  const auto* borrowed=queue.getNextToRead();queue.updateReadIndex();
#else
  const auto* borrowed=queue.peek();const auto local=*borrowed;queue.pop();
#endif
  phase.store(1,std::memory_order_release);
  while(phase.load(std::memory_order_acquire)!=2)std::this_thread::yield();
#ifdef OWNERSHIP_BASELINE
  forwarded=*borrowed;
#else
  forwarded=local;
#endif
  producer.join();
  std::cout<<"ownership expected=111 forwarded="<<forwarded<<" producer_next=222\n";
  require(forwarded==111,"released slot changed forwarded payload");
}

#ifndef OWNERSHIP_BASELINE
void position() {
  Common::Logger logger("position_check.log");
  Trading::PositionInfo stack;
  auto heap=std::unique_ptr<Trading::PositionInfo>(new Trading::PositionInfo);
  for(const auto* info:{&stack,heap.get()}) {
    require(info->position_==0 && info->total_pnl_==0,"default position initialization");
    for(auto cost:info->open_vwap_)require(cost==0,"default cost initialization");
  }
  Trading::PositionKeeper keeper(&logger);
  for(Common::TickerId i=0;i<Common::ME_MAX_TICKERS;++i) {
    const auto* info=keeper.getPositionInfo(i);
    require(info->position_==0 && info->real_pnl_==0 && info->volume_==0,"ticker position initialization");
    for(auto cost:info->open_vwap_)require(cost==0,"ticker cost initialization");
  }
  const Common::Side sides[]={Common::Side::BUY,Common::Side::SELL,Common::Side::SELL};
  const Common::Price prices[]={100,110,90};const Common::Qty quantities[]={10,4,8};
  const int64_t positions[]={10,6,-2};const double pnl[]={0,40,-20};
  for(size_t i=0;i<3;++i) {
    Exchange::MEClientResponse fill{Exchange::ClientResponseType::FILLED,1,0,i+1,i+1,sides[i],prices[i],quantities[i],0};
    stack.addFill(&fill,&logger);
    require(stack.position_==positions[i] && std::abs(stack.real_pnl_-pnl[i])<1e-9,"fill position or realized PnL");
    std::cout<<"fill "<<i+1<<" position="<<stack.position_<<" realized="<<stack.real_pnl_<<'\n';
  }
  require(stack.open_vwap_[Common::sideToIndex(Common::Side::BUY)]==0 &&
          stack.open_vwap_[Common::sideToIndex(Common::Side::SELL)]==180,"reverse position cost");
  std::cout<<"position heap+stack+8tickers initialized; short_cost=180\n";
}

void clear_book() {
  Common::Logger logger("clear_check.log");
  auto book=std::make_unique<Trading::MarketOrderBook>(0,&logger);
  require(book->liveOrders().empty(),"new book not empty");
  Exchange::MEMarketUpdate event{Exchange::MarketUpdateType::ADD,1,0,Common::Side::BUY,100,5,1};
  require(book->onMarketUpdate(&event),"first add");
  require(book->getBBO()->bid_price_==100 && book->getBBO()->bid_qty_==5,"first add BBO");
  event.order_id_=2;event.qty_=7;event.priority_=2;require(book->onMarketUpdate(&event),"second add");
  require(book->getBBO()->bid_qty_==12,"same price quantity");
  event.type_=Exchange::MarketUpdateType::CANCEL;event.order_id_=1;event.qty_=5;require(book->onMarketUpdate(&event),"first cancel");
  event.order_id_=2;event.qty_=7;require(book->onMarketUpdate(&event),"last cancel");
  require(book->liveOrders().empty() && book->getBBO()->bid_price_==Common::Price_INVALID,"last cancel BBO");
  event.type_=Exchange::MarketUpdateType::CLEAR;
  require(book->onMarketUpdate(&event) && book->onMarketUpdate(&event),"repeat clear");
  require(book->getBBO()->bid_price_==Common::Price_INVALID && book->getBBO()->ask_price_==Common::Price_INVALID &&
          book->getBBO()->bid_qty_==Trading::BookQty_INVALID && book->getBBO()->ask_qty_==Trading::BookQty_INVALID,"clear BBO sentinel");
  event={Exchange::MarketUpdateType::ADD,3,0,Common::Side::BUY,100,3,1};
  require(book->onMarketUpdate(&event) && book->getBBO()->bid_qty_==3 && book->liveOrders().size()==1,"same price after clear");
  std::cout<<"book first ADD -> same price -> last CANCEL -> CLEAR x2 -> rebuild PASS\n";
}
#endif

int main() {
  try {
    ownership();
#ifndef OWNERSHIP_BASELINE
    position();clear_book();
#endif
    std::cout<<"PASS review checks\n";return 0;
  } catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
