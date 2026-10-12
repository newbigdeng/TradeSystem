#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include "common/logging.h"
#include "common/mem_pool.h"
#include "exchange/order_server/fifo_sequencer.h"

void require(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
template<class F> void fails_closed(F f){
  const auto pid=fork();require(pid>=0,"fork failed");
  if(pid==0){f();_exit(0);}
  int status=0;require(waitpid(pid,&status,0)==pid,"wait failed");
  require(WIFEXITED(status)&&WEXITSTATUS(status)==EXIT_FAILURE,"invalid operation was not rejected");
}
struct alignas(64) Object {
  static inline int alive=0;
  explicit Object(int n):value(n){if(n<0)throw std::runtime_error("constructor failure");++alive;}
  ~Object(){--alive;}
  int value;
};
void pool_test(){
  {Common::MemPool<Object> pool(2);
   try{pool.allocate(-1);require(false,"constructor should throw");}catch(const std::runtime_error&){}
   require(pool.available()==2&&Object::alive==0,"throw consumed a pool slot");
   auto*a=pool.allocate(10);auto*b=pool.allocate(20);
   require(a&&b&&a!=b&&reinterpret_cast<uintptr_t>(a)%64==0&&reinterpret_cast<uintptr_t>(b)%64==0,"alignment or uniqueness");
   require(pool.allocate(30)==nullptr,"exhausted pool accepted object");pool.deallocate(a);
   auto*c=pool.allocate(30);require(c==a&&c->value==30&&Object::alive==2,"LIFO reuse/lifetime");
   pool.deallocate(b);require(Object::alive==1,"release skipped destructor");}
  require(Object::alive==0,"pool destructor leaked live objects");
  fails_closed([]{Common::MemPool<int> p(2);int foreign=0;p.deallocate(&foreign);});
  fails_closed([]{Common::MemPool<int> p(2);auto*v=p.allocate(1);p.deallocate(v);p.deallocate(v);});
  fails_closed([]{Common::MemPool<uint64_t> p(2);auto*v=p.allocate(1);p.deallocate(reinterpret_cast<uint64_t*>(reinterpret_cast<char*>(v)+1));});
  bool rejected=false;try{Common::MemPool<int> p(0);}catch(const std::invalid_argument&){rejected=true;}
  require(rejected,"zero capacity accepted");
#ifdef EXPECT_COMPACT_POOL
  rejected=false;try{Common::MemPool<int> p(size_t(UINT32_MAX)+1);}catch(const std::invalid_argument&){rejected=true;}
  require(rejected,"oversized pool must reject before allocation");
#endif
  std::cout<<"PASS pool alignment, constructor exception, full/reuse, destructors, foreign/double/interior pointer\n";
}
void queue_test(){
#ifdef EXPECT_POW2
  for(size_t n:{size_t(0),size_t(3),size_t(20),size_t(74),SIZE_MAX/2+1,SIZE_MAX}){
    bool rejected=false;try{Common::LFQueue<uint64_t> q(n);}catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"invalid queue capacity accepted or rejected too late");
  }
#endif
  for(size_t n:{1,2,4,8,1024}){
    Common::LFQueue<uint64_t> q(n);uint64_t value=0;
    for(size_t round=0;round<100;++round){
      for(size_t i=0;i<n;++i)require(q.try_push(round*n+i),"fill failed");
      auto* held=q.peek();require(held&&*held==round*n,"peek failed");
      require(!q.try_push(42)&&!q.try_push_batch({1}),"full queue overwrote data");
      require(*held==round*n,"failed admission changed borrowed element");
      for(size_t i=0;i<n;++i){require(q.try_pop(value)&&value==round*n+i,"wrap FIFO mismatch");}
      require(!q.peek()&&!q.try_pop(value),"empty read succeeded");
    }
    require(q.high_watermark()==n,"watermark changed");
    std::vector<uint64_t> too_big(n+1,42);require(!q.try_push_batch(too_big)&&!q.peek(),"oversized batch partially published");
    require(q.try_push_batch({}),"empty batch failed");
  }
  Common::LFQueue<int> wrapped(4);int value;
  for(int x:{1,2,3})require(wrapped.try_push(x),"prefill");
  for(int x:{1,2})require(wrapped.try_pop(value)&&value==x,"pre-drain");
  require(!wrapped.try_push_batch({4,5,6,7})&&wrapped.size()==1,"failed batch changed queue");
  require(wrapped.try_push_batch({4,5,6}),"wrapped batch failed");
  for(int x:{3,4,5,6})require(wrapped.try_pop(value)&&value==x,"wrapped batch order");
  struct Packet{uint64_t id,inverse;};Common::LFQueue<Packet> shared(8);constexpr uint64_t count=1000000;
  std::thread producer([&]{std::vector<Packet> batch;for(uint64_t i=0;i<count;){
    batch.clear();const auto n=std::min<uint64_t>(1+i%8,count-i);
    for(uint64_t j=0;j<n;++j)batch.push_back({i+j,~(i+j)});
    while(!shared.try_push_batch(batch)){std::this_thread::yield();}
    i+=n;
  }});
  bool correct=true;for(uint64_t i=0;i<count;++i){const Packet*p=nullptr;while(!(p=shared.peek()))std::this_thread::yield();
    correct&=p->id==i&&p->inverse==~i;shared.pop();}
  producer.join();require(correct&&!shared.peek(),"concurrent batch loss/reorder/torn payload");
  std::cout<<"PASS queue invalid capacities, full/empty, 100 wraps, atomic batches, 1000000 ordered packets\n";
}
void fifo_test(){
  Common::Logger logger("fifo_regression.log");
  for(size_t capacity:{1,2,8,32}){
    Exchange::ClientRequestLFQueue q(capacity);Exchange::FIFOSequencer fifo(&q,&logger);
    Exchange::MEClientRequest sentinel{};sentinel.order_id_=999;
    for(size_t i=0;i<capacity;++i)require(q.try_push(sentinel),"prefill FIFO downstream");
    std::vector<std::pair<Common::Nanos,uint64_t>> expected;
    for(uint64_t i=0;i<97;++i){auto r=sentinel;r.order_id_=i+1;auto time=Common::Nanos((i*37)%11);
      require(fifo.addClientRequest(time,r),"pending admission failed");expected.emplace_back(time,i+1);}
    std::stable_sort(expected.begin(),expected.end(),[](auto a,auto b){return a.first<b.first;});
    fifo.sequenceAndPublish();fifo.sequenceAndPublish();
    require(fifo.pending()==97,"i=0 retry lost pending messages");
    Exchange::MEClientRequest value;
    for(size_t i=0;i<capacity;++i)require(q.try_pop(value)&&value.order_id_==999,"full retry changed queued data");
    size_t next=0;
    while(fifo.pending()){
      fifo.sequenceAndPublish();const auto pending=fifo.pending();fifo.sequenceAndPublish();
      require(fifo.pending()==pending,"repeated full retry changed count");
      while(q.try_pop(value)){require(next<expected.size()&&value.order_id_==expected[next++].second,"partial retry reordered/duplicated");}
    }
    require(next==expected.size()&&!q.peek(),"FIFO lost requests");
  }
  std::cout<<"PASS FIFO first-full, partial publication, repeated backpressure, timestamp ties and complete drain\n";
}
int main(int argc,char**argv){try{const std::string mode=argc>1?argv[1]:"all";
  if(mode=="all"||mode=="pool")pool_test();
  if(mode=="all"||mode=="queue")queue_test();
  if(mode=="all"||mode=="fifo")fifo_test();
  return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
