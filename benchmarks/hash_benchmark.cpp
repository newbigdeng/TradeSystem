#include "matcher/me_order_book.h"
#include "matcher/unordered_map_me_order_book.h"
#include "order_server/client_request.h"
#include "record/02_reproducible_measurement/benchmark_support.h"
#include <array>

using namespace Common;using namespace Exchange;
const std::array<std::string,12> labels={"empty_new","second_level","same_price_fifo","partial_fill",
  "multi_level_fill","cancel_success","cancel_failure","bid_356","bid_100_collision",
  "cancel_collision","one_level_fill","cancel_filled"};
std::vector<MEClientRequest> input() {
  std::vector<MEClientRequest> rows;rows.reserve(550*12);
  for(uint64_t cycle=0;cycle<550;++cycle) {
    const auto id=cycle*10;
    auto add=[&](uint32_t c,uint64_t o,Side side,Price price,Qty qty) {
      rows.push_back({ClientRequestType::NEW,c,0,id+o,side,price,qty});};
    auto cancel=[&](uint32_t c,uint64_t o) {rows.push_back({ClientRequestType::CANCEL,c,0,id+o,Side::INVALID,Price_INVALID,0});};
    add(1,1,Side::SELL,100,3);add(1,2,Side::SELL,356,4);add(1,3,Side::SELL,100,2);
    add(2,4,Side::BUY,100,2);add(2,5,Side::BUY,356,6);cancel(1,2);cancel(1,1);
    add(1,6,Side::BUY,356,3);add(1,7,Side::BUY,100,1);cancel(1,7);
    add(2,8,Side::SELL,356,3);cancel(1,6);
  }
  return rows;
}
struct Result {std::vector<std::string> responses,updates;};
template<class Book> Result benchmark(const std::string& name,const std::vector<MEClientRequest>& rows) {
  Logger logger(name+".log");Book book(0,&logger,nullptr);Result result;
  result.responses.reserve(550*22);result.updates.reserve(550*17);
  uint64_t accepted=0,canceled=0,rejected=0,filled=0,volume=0;int64_t balance=0;
  book.response_sink_=[&](const MEClientResponse& r) {
    result.responses.push_back(r.toString());
    accepted+=r.type_==ClientResponseType::ACCEPTED;canceled+=r.type_==ClientResponseType::CANCELED;
    rejected+=r.type_==ClientResponseType::CANCEL_REJECTED;filled+=r.type_==ClientResponseType::FILLED;
    if(r.type_==ClientResponseType::FILLED){volume+=r.exec_qty_;balance+=int64_t(r.exec_qty_)*sideToValue(r.side_);}
  };
  book.market_sink_=[&](const MEMarketUpdate& u){result.updates.push_back(u.toString());};
  std::vector<uint64_t> all(6000);std::array<std::vector<uint64_t>,12> cases;
  for(auto& values:cases)values.reserve(500);
  uint64_t window=0;
  for(size_t i=0;i<rows.size();++i) {
    if(i==600)window=Measure::now();
    const auto& r=rows[i];const auto start=Measure::now();
    if(r.type_==ClientRequestType::NEW)book.add(r.client_id_,r.order_id_,0,r.side_,r.price_,r.qty_);
    else book.cancel(r.client_id_,r.order_id_,0);
    const auto ns=Measure::now()-start;
    if(i>=600){all[i-600]=ns;cases[i%12].push_back(ns);}
    if(i%12==11)Measure::require(book.liveOrders().empty(),"scenario left live orders");
  }
  const auto elapsed=Measure::now()-window;
  Measure::require(accepted==550*8 && canceled==550*2 && rejected==550*2 && filled==550*10
    && volume==550*22 && balance==0 && result.updates.size()==550*17,"business count or quantity conservation differs");
  Measure::output(name,all,elapsed,volume);
  for(size_t i=0;i<12;++i)Measure::output(name+"_"+labels[i],cases[i],elapsed,volume);
  std::cout<<"counts accepted="<<accepted<<" canceled="<<canceled<<" explicit_cancel_rejected="<<rejected
    <<" fill_responses="<<filled<<" updates="<<result.updates.size()<<" final_live=0 max_live_depth=3\n";
  return result;
}
int main(int argc,char**) {
  try {
    const auto rows=input();std::ofstream file("book_input.csv");
    file<<"type,client,ticker,order,side,price,qty\n";
    for(const auto& r:rows)file<<int(r.type_)<<','<<r.client_id_<<",0,"<<r.order_id_<<','<<int(r.side_)<<','<<r.price_<<','<<r.qty_<<'\n';
    file.close();Measure::clockOverhead();
    std::cout<<"NOTE component call -> all sink events emitted; includes ordinary diagnostic logging and validating sinks, excludes network and durable journal. Exactly one timed call per sample.\n";
    Result first,second;
    if(argc>1) {
      second=benchmark<UnorderedMapMEOrderBook>("book_alternative",rows);
      first=benchmark<MEOrderBook>("book_primary",rows);
    } else {
      first=benchmark<MEOrderBook>("book_primary",rows);
      second=benchmark<UnorderedMapMEOrderBook>("book_alternative",rows);
    }
    Measure::require(first.responses==second.responses && first.updates==second.updates,"implementations produced different business events");
    return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
