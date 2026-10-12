#include <fstream>
#include <sys/wait.h>
#include "matcher/matching_engine.h"
int main() {
  char directory[]="/tmp/trade-journal-test-XXXXXX";ASSERT(mkdtemp(directory)!=nullptr,"mkdtemp failed");
  const std::string path=std::string(directory)+"/orders.journal";
  {
    Common::CriticalJournal journal(path);journal.append("TEST persisted");
    auto pid=fork();ASSERT(pid>=0,"fork failed");
    if(pid==0) {Common::CriticalJournal forbidden(path);std::_Exit(9);}
    int status=0;while(waitpid(pid,&status,0)<0&&errno==EINTR){}
    ASSERT(WIFEXITED(status)&&WEXITSTATUS(status)==1,"restart overwrote an existing journal");
    pid=fork();ASSERT(pid>=0,"second fork failed");
    if(pid==0) {
      Exchange::ClientRequestLFQueue requests(1);Exchange::ClientResponseLFQueue responses(1);Exchange::MEMarketUpdateLFQueue updates(1);
      Exchange::MatchingEngine engine(&requests,&responses,&updates,&journal);
      const Exchange::MEClientResponse response{Exchange::ClientResponseType::ACCEPTED,1,0,1,1,Common::Side::BUY,100,0,1};
      engine.sendClientResponse(&response);engine.sendClientResponse(&response);std::_Exit(9);
    }
    while(waitpid(pid,&status,0)<0&&errno==EINTR){}
    ASSERT(WIFEXITED(status)&&WEXITSTATUS(status)==1,"critical queue overflow did not fail closed");
    std::ifstream input(path);std::string line;size_t responses=0;bool test=false;
    while(std::getline(input,line)){test|=line=="TEST persisted";responses+=line.starts_with("RESPONSE ");}
    ASSERT(test&&responses==2,"critical outputs were not durably retained before overflow");
  }
  {
    Common::Logger logger("query_test.log");Exchange::MEOrderBook book(0,&logger,nullptr);
    Exchange::MEClientResponse result;
    book.response_sink_=[&](const auto& response){result=response;};book.market_sink_=[](const auto&){};
    book.add(1,1,0,Common::Side::BUY,100,3);book.query(1,1);
    ASSERT(result.type_==Exchange::ClientResponseType::STATE&&result.leaves_qty_==3,"live query differs");
    book.cancel(1,1,0);book.query(1,1);
    ASSERT(result.type_==Exchange::ClientResponseType::STATE&&result.leaves_qty_==0,"query revived canceled leaves");
  }
  ASSERT(unlink(path.c_str())==0&&rmdir(directory)==0,"test cleanup failed");
  std::cout<<"PASS journal persistence, restart fence, critical overflow audit\n";
}
