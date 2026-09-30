#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include "matcher/matching_engine.h"
#include "matcher/unordered_map_me_order_book.h"
template<class T> bool take(Common::LFQueue<T>& q,T& v) {
#ifdef TRADE_BASELINE
  auto *p=q.getNextToRead();if(!p)return false;v=*p;q.updateReadIndex();return true;
#else
  return q.try_pop(v);
#endif
}
template<class Book> void execute(Book* book,Exchange::ClientResponseLFQueue& responses,Exchange::MEMarketUpdateLFQueue& updates,const std::string& mode) {
  if(mode=="collision") {
    book->add(1,1,0,Common::Side::BUY,100,3);
    book->add(1,2,0,Common::Side::BUY,356,7);
    book->cancel(1,1,0);
    const auto text=book->toString(true,true);
    if(text.find("356")==std::string::npos || text.find("100(")!=std::string::npos) throw std::runtime_error("price collision mixed two distinct levels");
    book->add(2,3,0,Common::Side::SELL,300,7);
    Exchange::MEClientResponse r;uint64_t fills=0;
    while(take(responses,r)) if(r.type_==Exchange::ClientResponseType::FILLED) fills+=r.exec_qty_;
    if(fills!=14) throw std::runtime_error("cross-price execution or quantity conservation failed");
    std::cout<<"PASS collision and quantity conservation\n";
  } else {
    constexpr uint64_t count=20000;uint64_t checksum=0;
    const auto begin=std::chrono::steady_clock::now();
    for(uint64_t i=0;i<count;++i) {
      book->add(1,i+1,0,Common::Side::BUY,100+(i%10),3);
      book->cancel(1,i+1,0);
      Exchange::MEClientResponse r; while(take(responses,r)) checksum+=r.type_==Exchange::ClientResponseType::ACCEPTED;
      Exchange::MEMarketUpdate u;while(take(updates,u)) checksum+=u.type_==Exchange::MarketUpdateType::ADD;
    }
    if(checksum!=count*2) throw std::runtime_error("book benchmark output count differs");
    const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count();
    std::cout<<"{\"mode\":\"book\",\"operations\":"<<count*2<<",\"elapsed_ns\":"<<ns
             <<",\"ns_per_operation\":"<<double(ns)/(count*2)<<",\"checksum\":"<<checksum
             <<",\"sizeof_book\":"<<sizeof(Book)<<"}\n";
    std::ifstream status("/proc/self/status");std::string line;
    while(std::getline(status,line)) if(line.starts_with("VmSize:")||line.starts_with("VmRSS:")) std::cerr<<line<<'\n';
  }
}
int main(int argc,char** argv) {
  try {
    if(argc!=3)return 2;
    Common::Logger logger("book_probe.log");
    Exchange::ClientRequestLFQueue requests(1024);Exchange::ClientResponseLFQueue responses(1024);Exchange::MEMarketUpdateLFQueue updates(1024);
    auto *engine=new Exchange::MatchingEngine(&requests,&responses,&updates);
    if(std::string(argv[2])=="unordered")execute(new Exchange::UnorderedMapMEOrderBook(0,&logger,engine),responses,updates,argv[1]);
    else execute(new Exchange::MEOrderBook(0,&logger,engine),responses,updates,argv[1]);
    std::cout.flush();std::cerr.flush();std::_Exit(0); // baseline destructor touches the 18 GiB direct pointer tables
  } catch(const std::exception& e) {std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
