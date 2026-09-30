#include <fstream>
#include <stdexcept>
#include <thread>
#include "common/logging.h"
void require(bool value,const char *message) { if(!value) throw std::runtime_error(message); }
int main() {
  for(size_t capacity:{1,2,3}) {
    Common::LFQueue<uint64_t> q(capacity);
    for(size_t i=0;i<capacity;++i) require(q.try_push(i),"initial push failed");
    require(!q.try_push(999),"full queue accepted a message");
    for(size_t i=0;i<capacity;++i) {uint64_t value=999; require(q.try_pop(value)&&value==i,"FIFO changed");}
    uint64_t value=0; require(!q.try_pop(value),"empty pop succeeded");
  }
  for(int repeat=0;repeat<3;++repeat) {
    Common::LFQueue<uint64_t> q(3); constexpr uint64_t count=1000000;
    std::thread producer([&]{ for(uint64_t i=0;i<count;++i) {
      while(!q.try_push(i)) std::this_thread::yield();
      if(i%997==0) std::this_thread::yield();
    }});
    bool ordered=true;
    for(uint64_t i=0;i<count;++i) {uint64_t value; while(!q.try_pop(value)) std::this_thread::yield(); ordered&=value==i;}
    producer.join(); require(ordered&&q.size()==0,"million-message transfer differs");
    require(q.high_watermark()==3,"queue high watermark missing");
  }
  uint64_t dropped=0;
  {
    Common::Logger logger("bounded_logger_test.log",1);
    std::vector<std::thread> writers;
    for(int w=0;w<4;++w) writers.emplace_back([&logger,w]{for(int i=0;i<1000;++i) logger.log("record:%,%\n",w,i);});
    for(auto &writer:writers) writer.join();
    dropped=logger.dropped_records();
    require(dropped>0,"logger overflow not observed");
  }
  std::ifstream input("bounded_logger_test.log"); std::string line; uint64_t records=0;
  while(std::getline(input,line)) if(line.starts_with("record:")) ++records;
  require(records+dropped==4000,"logger lost part of a record without counting it");
  std::cout<<"PASS bounded queue, 3 x 1000000 SPSC messages, logger accepted+dropped=4000\n";
}
