#include <array>
#include "matcher/me_order_book.h"
#include "matcher/unordered_map_me_order_book.h"
#include "record/02_reproducible_measurement/benchmark_support.h"
using namespace Common;using namespace Exchange;
struct Result {std::vector<std::string> responses,updates;uint64_t accepted=0,canceled=0,filled=0,volume=0;int64_t balance=0;};
template<class Book> Result run(const std::string& name,const std::string& scenario) {
  Logger logger(name+".log");Book book(0,&logger,nullptr);Result result;
  book.response_sink_=[&](const MEClientResponse& r) {
    result.responses.push_back(r.toString());result.accepted+=r.type_==ClientResponseType::ACCEPTED;result.canceled+=r.type_==ClientResponseType::CANCELED;result.filled+=r.type_==ClientResponseType::FILLED;
    if(r.type_==ClientResponseType::FILLED){result.volume+=r.exec_qty_;result.balance+=int64_t(r.exec_qty_)*sideToValue(r.side_);}
  };
  book.market_sink_=[&](const MEMarketUpdate& e){result.updates.push_back(e.toString());};
  std::vector<uint64_t> add,cancel,fill;uint64_t next=1;const auto start=Measure::now();
  if(scenario=="dense"||scenario=="sparse") {
    for(int cycle=0;cycle<105;++cycle) {
      const auto first=next;
      for(int i=0;i<128;++i) {
        const auto a=Measure::now();book.add(1,next++,0,Side::BUY,scenario=="dense"?100:100+256*i,1);
        if(cycle>=5)add.push_back(Measure::now()-a);
      }
      for(int i=0;i<128;++i) {
        const auto a=Measure::now();book.cancel(1,first+i,0);if(cycle>=5)cancel.push_back(Measure::now()-a);
      }
      Measure::require(book.liveOrders().empty(),"dense/sparse batch left live orders");
    }
    Measure::require(result.accepted==105*128&&result.canceled==105*128&&result.filled==0,"dense/sparse outcome mismatch");
  }else {
    const int levels=scenario=="one_level"?1:8;
    for(int cycle=0;cycle<2050;++cycle) {
      for(int i=0;i<levels;++i)book.add(1,next++,0,Side::SELL,100+256*i,4);
      const auto a=Measure::now();book.add(2,next++,0,Side::BUY,100+256*(levels-1),4*levels);
      if(cycle>=50)fill.push_back(Measure::now()-a);
      Measure::require(book.liveOrders().empty(),"crossing batch left live orders");
    }
    Measure::require(result.accepted==2050ULL*(levels+1)&&result.filled==2050ULL*2*levels&&result.volume==2050ULL*8*levels&&result.balance==0,"fill volume/conservation mismatch");
  }
  const auto elapsed=Measure::now()-start;
  if(!add.empty()){Measure::output(name+"_new",add,elapsed,result.accepted);Measure::output(name+"_cancel",cancel,elapsed,result.canceled);}
  if(!fill.empty())Measure::output(name+"_crossing_new",fill,elapsed,result.volume);
  std::cout<<"{\"control\":\""<<name<<"\",\"accepted\":"<<result.accepted<<",\"canceled\":"<<result.canceled<<",\"fill_responses\":"<<result.filled
    <<",\"paired_execution_quantity\":"<<result.volume<<",\"signed_balance\":"<<result.balance<<",\"final_live\":0}\n";
  return result;
}
int main(int argc,char**) {
  try {
    Measure::clockOverhead();
    for(const std::string scenario:{"dense","sparse","one_level","eight_levels"}) {
      Result a,b;
      if(argc>1){b=run<UnorderedMapMEOrderBook>("alternative_"+scenario,scenario);a=run<MEOrderBook>("primary_"+scenario,scenario);}
      else {a=run<MEOrderBook>("primary_"+scenario,scenario);b=run<UnorderedMapMEOrderBook>("alternative_"+scenario,scenario);}
      Measure::require(a.responses==b.responses&&a.updates==b.updates,"book business/MD outputs diverge");
    }
    return 0;
  }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
