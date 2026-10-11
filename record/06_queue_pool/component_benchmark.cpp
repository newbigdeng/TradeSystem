#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>
#include <thread>
#include <pthread.h>
#include <sched.h>
#include "common/lf_queue.h"
#include "common/mem_pool.h"
#include "exchange/matcher/me_order.h"

static std::atomic<uint64_t> allocations{0}, allocated_bytes{0};
static std::atomic<bool> count_allocations{false};
void* operator new(std::size_t n) {
  if(count_allocations.load(std::memory_order_relaxed)) {
    allocations.fetch_add(1,std::memory_order_relaxed);
    allocated_bytes.fetch_add(n,std::memory_order_relaxed);
  }
  if(void* p=std::malloc(n?n:1))return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
using Clock=std::chrono::steady_clock;
uint64_t now(){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();}
void require(bool ok){if(!ok)throw std::runtime_error("benchmark correctness failure");}
void pin(int cpu){cpu_set_t set;CPU_ZERO(&set);CPU_SET(cpu,&set);require(pthread_setaffinity_np(pthread_self(),sizeof(set),&set)==0);}
void output(const char* name,uint64_t count,uint64_t elapsed,uint64_t sum){
  std::printf("{\"mode\":\"%s\",\"count\":%lu,\"ns\":%lu,\"ns_per_op\":%.6f,\"checksum\":%lu}\n",name,count,elapsed,double(elapsed)/count,sum);
}
// Both consumer-loop shapes remain in every binary, so removing size() can be
// compared within the same process rather than credited to a different build.
template<bool SizeCheck> void local_queue(size_t capacity){
  Common::LFQueue<uint64_t> q(capacity);constexpr uint64_t count=1000000;uint64_t sum=0;
  auto loop=[&](uint64_t n){for(uint64_t i=0;i<n;++i){require(q.try_push(i));const auto*p=q.peek();
    if constexpr(SizeCheck)require(q.size()!=0);
    require(p&&*p==i);sum+=*p;q.pop();}};
  loop(10000);sum=0;auto start=now();loop(count);auto elapsed=now()-start;
  require(sum==count*(count-1)/2);output(SizeCheck?"queue_size_peek_pop":"queue_peek_pop",count,elapsed,sum);
}
void shared_queue(size_t capacity){
  struct Packet {uint64_t id,a,b;};Common::LFQueue<Packet> q(capacity);constexpr uint64_t count=2000000;
  std::atomic<bool> ready{false},start{false};uint64_t sum=0;
  std::thread consumer([&]{pin(1);ready=true;while(!start.load())std::this_thread::yield();
    for(uint64_t i=0;i<count;++i){const Packet*p=nullptr;while(!(p=q.peek())){}
      require(p->id==i&&p->a==(i^0x12345678)&&p->b==i+7);sum+=p->id;q.pop();}});
  while(!ready.load()){std::this_thread::yield();}
  auto begin=now();start=true;
  for(uint64_t i=0;i<count;++i)while(!q.try_push({i,i^0x12345678,i+7})){}
  consumer.join();auto elapsed=now()-begin;require(sum==count*(count-1)/2&&!q.peek());
  output("queue_spsc_24B",count,elapsed,sum);
}
void pool(){
  constexpr size_t slots=4096,rounds=256;Common::MemPool<Exchange::MEOrder> p(slots);
  std::vector<Exchange::MEOrder*> live(slots);uint64_t sum=0;
  auto loop=[&](size_t n){for(size_t r=0;r<n;++r){for(size_t i=0;i<slots;++i){
    live[i]=p.allocate();require(live[i]!=nullptr);live[i]->client_order_id_=i;}
    // Visit each slot once in a deterministic nonsequential release order.
    for(size_t i=0;i<slots;++i){auto* item=live[(i*2053)&(slots-1)];sum+=item->client_order_id_;p.deallocate(item);}}};
  loop(2);sum=0;auto begin=now();loop(rounds);auto elapsed=now()-begin;
  require(sum==rounds*slots*(slots-1)/2&&p.available()==slots);output("pool_MEOrder",rounds*slots,elapsed,sum);
}
void allocation_counts(){
  Common::MemPool<uint64_t> pool(4);Common::LFQueue<uint64_t> q(4);
  allocations=0;count_allocations=true;
  for(int i=0;i<10000;++i){auto*p=pool.allocate(i);pool.deallocate(p);}
  count_allocations=false;std::printf("{\"mode\":\"pool_allocations\",\"count\":10000,\"new_calls\":%lu}\n",allocations.load());
  allocations=0;count_allocations=true;
  for(int i=0;i<10000;++i){require(q.try_push(i));require(q.peek()!=nullptr);q.pop();}
  count_allocations=false;std::printf("{\"mode\":\"queue_allocations\",\"count\":10000,\"new_calls\":%lu}\n",allocations.load());
  allocations=0;allocated_bytes=0;count_allocations=true;
  {Common::MemPool<Exchange::MEOrder> large(1024*1024);count_allocations=false;require(large.available()==1024*1024);}
  std::printf("{\"mode\":\"pool_storage\",\"slots\":1048576,\"new_calls\":%lu,\"bytes\":%lu}\n",allocations.load(),allocated_bytes.load());
}
int main(int argc,char**argv){try{pin(0);const size_t capacity=argc>1?std::stoull(argv[1]):1024;
  local_queue<true>(capacity);local_queue<false>(capacity);shared_queue(capacity);pool();allocation_counts();return 0;
}catch(const std::exception&e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
