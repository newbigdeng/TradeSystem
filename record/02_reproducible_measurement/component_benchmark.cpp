#include "benchmark_support.h"
#include "common/lf_queue.h"
#include "common/tcp_socket.h"
#include "trading/market_data/market_recovery.h"
#include <array>
#include <atomic>
#include <cstdlib>
#include <pthread.h>
#include <sched.h>
#include <thread>

void pin(const char* variable) {
  if(const auto* cpu=std::getenv(variable)) {
    const auto number=std::stoi(cpu);Measure::require(number>=0 && number<CPU_SETSIZE,"invalid affinity");
    cpu_set_t mask;CPU_ZERO(&mask);CPU_SET(number,&mask);
    Measure::require(pthread_setaffinity_np(pthread_self(),sizeof(mask),&mask)==0,"affinity failed");
  }
}
void queue() {
  constexpr uint64_t count=100000;Common::LFQueue<uint64_t> local(1024);
  uint64_t value=0,checksum=0;std::vector<uint64_t> calls(count);
  for(uint64_t i=0;i<10000;++i)Measure::require(local.try_push(i)&&local.try_pop(value)&&value==i,"warmup queue mismatch");
  auto window=Measure::now();
  for(uint64_t i=0;i<count;++i){const auto start=Measure::now();
    const auto pushed=local.try_push(i),popped=local.try_pop(value);calls[i]=Measure::now()-start;
    Measure::require(pushed&&popped&&value==i,"queue pair mismatch");checksum+=value;}
  Measure::output("queue_pair",calls,Measure::now()-window,checksum);
  struct Packet {uint64_t id,start;};Common::LFQueue<Packet> shared(1024);
  std::vector<uint64_t> transfer(count);std::atomic<bool> ready{false};uint64_t sum=0;
  std::thread consumer([&]{pin("TRADE_CONSUMER_CPU");ready.store(true,std::memory_order_release);
    for(uint64_t i=0;i<count;++i){Packet packet{};
      while(!shared.try_pop(packet))std::this_thread::yield();
      transfer[i]=Measure::now()-packet.start;
      Measure::require(packet.id==i,"cross-thread queue sequence mismatch");sum+=packet.id;}
  });
  pin("TRADE_PRODUCER_CPU");while(!ready.load(std::memory_order_acquire))std::this_thread::yield();
  window=Measure::now();
  for(uint64_t i=0;i<count;++i)while(!shared.try_push({i,Measure::now()}))std::this_thread::yield();
  consumer.join();
  Measure::require(sum==count*(count-1)/2 && shared.size()==0,"queue loss or duplication");
  Measure::output("queue_attempt_to_consume",transfer,Measure::now()-window,sum);
  std::cout<<"queue_full_retries="<<shared.full_count()<<" queue_high_watermark="<<shared.high_watermark()<<'\n';
}
void tcp() {
  Common::Logger logger("tcp_append.log");Common::TCPSocket socket(logger);
  const std::array<char,512> payload=[] {std::array<char,512> a{};for(size_t i=0;i<a.size();++i)a[i]=char(i%127);return a;}();
  std::vector<uint64_t> calls(100000);uint64_t checksum=0;
  const auto window=Measure::now();
  for(size_t i=0;i<calls.size();++i) {
    if(i%1024==0)socket.next_send_valid_index_=0; // Only reset this standalone buffer; no kernel writes occur.
    const auto start=Measure::now();const auto result=socket.send(payload.data(),payload.size());calls[i]=Measure::now()-start;
    Measure::require(result==Common::SendResult::Accepted,"TCP append failed");
    Measure::require(std::equal(payload.begin(),payload.end(),socket.outbound_data_.begin()+socket.pending_bytes()-payload.size()),"TCP append bytes differ");
    checksum+=socket.pending_bytes();
  }
  Measure::output("tcp_buffer_append",calls,Measure::now()-window,checksum);
}
using Frame=Exchange::MDPMarketUpdate;using Event=Exchange::MEMarketUpdate;using Type=Exchange::MarketUpdateType;
std::vector<Frame> snapshot(uint64_t cycle) {
  std::vector<Event> orders;for(uint64_t i=1;i<=64;++i)orders.push_back({Type::ADD,i,Common::TickerId(i%8),Common::Side::BUY,100,1,i});
  const auto hash=Common::bookHash(orders);std::vector<Frame> frames;
  auto add=[&](Event event){frames.push_back({frames.size(),event,cycle,42,hash});};
  add({Type::SNAPSHOT_START,42});for(Common::TickerId i=0;i<8;++i)add({Type::CLEAR,Common::OrderId_INVALID,i});
  for(const auto& order:orders)add(order);
  add({Type::SNAPSHOT_END,42});return frames;
}
void recovery() {
  std::vector<uint64_t> samples(1000);uint64_t checksum=0;Trading::MarketRecovery recovery;
  const auto frames=snapshot(1);uint64_t window=0;
  for(size_t round=0;round<1100;++round) {
    if(round==100)window=Measure::now();
    auto input=frames;for(auto& frame:input)frame.snapshot_cycle_=round+1;
    const auto start=Measure::now();recovery.restart();
    for(const auto& frame:input)recovery.ingest(true,frame);
    const auto batch=recovery.build();Measure::require(bool(batch),"complete snapshot did not recover");recovery.commit(*batch);
    const auto ns=Measure::now()-start;if(round>=100){samples[round-100]=ns;checksum+=batch->events.size();}
    Measure::require(!recovery.recovering()&&recovery.expected()==43&&batch->events.size()==72,"recovery state mismatch");
  }
  Measure::output("recovery_snapshot_component",samples,Measure::now()-window,checksum);
  std::cout<<"snapshot_frames="<<frames.size()<<" order_count=64 tickers=8 watermark=42; excludes network wait and downstream commit\n";
}
int main(int argc,char** argv) {
  try {
    Measure::clockOverhead();const std::string mode=argc>1?argv[1]:"all";
    if(mode=="all"||mode=="queue")queue();
    if(mode=="all"||mode=="tcp")tcp();
    if(mode=="all"||mode=="recovery")recovery();
    Measure::require(mode=="all"||mode=="queue"||mode=="tcp"||mode=="recovery","unknown mode");
    return 0;
  }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
