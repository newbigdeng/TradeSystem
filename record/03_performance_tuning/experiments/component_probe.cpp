#include <atomic>
#include <array>
#include <cstring>
#include <thread>
#include <sched.h>
#include "record/02_reproducible_measurement/benchmark_support.h"
#include "common/lf_queue.h"
#include "common/tcp_socket.h"
#include "common/market_protocol.h"
#include "common/book_hash.h"
#include "exchange/market_data/snapshot_synthesizer.h"
#include "trading/market_data/market_recovery.h"
using namespace Common;
using namespace Exchange;
void pin(int cpu) {cpu_set_t set;CPU_ZERO(&set);CPU_SET(cpu,&set);Measure::require(!sched_setaffinity(0,sizeof(set),&set),"affinity failed");}
struct Item {uint64_t sequence,first_try,publish;};
void queueProbe() {
  constexpr uint64_t count=100000;
  for(size_t capacity:{size_t(128),size_t(1024),size_t(8192)}) {
    LFQueue<Item> q(capacity);std::vector<uint64_t> wait(count),backpressure(count);
    std::atomic<bool> ready{false};bool correct=true;uint64_t checksum=0;
    std::thread consumer([&] {
      pin(1);ready=true;
      for(uint64_t i=0;i<count;++i) {
        if(i%10000==0)std::this_thread::sleep_for(std::chrono::milliseconds(10));
        Item item{};while(!q.try_pop(item)) {}
        const auto taken=Measure::now();correct&=item.sequence==i;
        checksum+=item.sequence;wait[i]=taken-item.publish;backpressure[i]=item.publish-item.first_try;
      }
    });
    pin(0);while(!ready){}const auto start=Measure::now();
    for(uint64_t i=0;i<count;++i) {
      Item item{i,Measure::now(),0};do {item.publish=Measure::now();}while(!q.try_push(item));
    }
    consumer.join();const auto elapsed=Measure::now()-start;
    Measure::require(correct&&q.size()==0&&checksum==count*(count-1)/2,"queue lost/reordered data");
    const auto name="queue_capacity_"+std::to_string(capacity);
    Measure::output(name+"_residence",wait,elapsed,checksum);
    Measure::output(name+"_producer_wait",backpressure,elapsed,checksum);
    std::cout<<"{\"control\":\""<<name<<"\",\"capacity\":"<<capacity<<",\"slot_bytes\":"<<capacity*sizeof(Item)
      <<",\"highwater\":"<<q.high_watermark()<<",\"full_attempts\":"<<q.full_count()<<",\"final\":0,\"injected_consumer_pause_ms\":100}\n";
  }
}
void tcpProbe() {
  Logger logger("tcp_probe.log");
  for(int pause:{0,20,100}) {
    std::vector<uint64_t> totals,appends;
    uint64_t total_full=0,total_eagain=0,max_pending=0,checksum=0;
    for(int round=0;round<5;++round) {
      int listener=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
      sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
      Measure::require(bind(listener,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0&&listen(listener,1)==0,"TCP listen");
      socklen_t length=sizeof(addr);getsockname(listener,reinterpret_cast<sockaddr*>(&addr),&length);
      const int peer=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);
      Measure::require(connect(peer,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0,"TCP connect");
      const int fd=accept(listener,nullptr,nullptr);close(listener);
      setNonBlocking(fd);setNonBlocking(peer);disableNagle(fd);disableNagle(peer);
      int small=4096,receive_limit=65536;setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&small,sizeof(small));setsockopt(peer,SOL_SOCKET,SO_RCVBUF,&receive_limit,sizeof(receive_limit));
      TCPSocket tx(logger,65536);tx.socket_fd_=fd;
      std::vector<char> input(79*4096),output(input.size());
      for(size_t i=0;i<input.size();++i)input[i]=char((i*31+7)%251);
      std::atomic<bool> go{false};bool complete=false;
      std::thread receiver([&] {
        pin(1);while(!go){}std::this_thread::sleep_for(std::chrono::milliseconds(pause));size_t offset=0;
        const auto deadline=Measure::now()+10*NANOS_TO_SECS;
        while(offset<output.size()&&Measure::now()<deadline) {
          const auto n=recv(peer,output.data()+offset,output.size()-offset,MSG_DONTWAIT);
          if(n>0)offset+=n;
        }
        complete=offset==output.size();
      });
      pin(0);const auto start=Measure::now();go=true;size_t offset=0;uint64_t full=0;
      bool timed_out=false;
      while(offset<input.size()||tx.pending_bytes()) {
        if(offset<input.size()) {
          const auto a=Measure::now();const auto status=tx.send(input.data()+offset,79);const auto ns=Measure::now()-a;
          if(status==SendResult::Accepted){offset+=79;appends.push_back(ns);}else if(status==SendResult::Full)++full;else Measure::require(false,"TCP terminal while sending");
        }
        max_pending=std::max<uint64_t>(max_pending,tx.pending_bytes());tx.sendAndRecv();
        if(Measure::now()-start>=10*NANOS_TO_SECS){timed_out=true;break;}
      }
      receiver.join();totals.push_back(Measure::now()-start);close(peer);
      const auto eof_deadline=Measure::now()+NANOS_TO_SECS;
      while(tx.healthy()&&Measure::now()<eof_deadline)tx.sendAndRecv();
      Measure::require(!timed_out&&complete&&input==output&&tx.pending_bytes()==0&&tx.state_==ConnectionState::PeerClosed,"TCP byte/EOF mismatch");
      total_full+=full;total_eagain+=tx.send_would_block_;checksum+=input.size();
    }
    uint64_t sum=0;for(auto v:totals)sum+=v;
    const auto name="tcp_reader_pause_"+std::to_string(pause);
    Measure::output(name+"_transfer",totals,sum,checksum);
    Measure::output(name+"_accepted_buffer_append",appends,sum,checksum);
    std::cout<<"{\"control\":\""<<name<<"\",\"bytes_per_transfer\":"<<79*4096<<",\"full_attempts\":"<<total_full
      <<",\"send_eagain\":"<<total_eagain<<",\"max_pending\":"<<max_pending<<",\"byte_exact\":true,\"final_pending\":0,\"requested_sndbuf\":4096,\"requested_rcvbuf\":65536}\n";
  }
}
void snapshotProbe() {
  for(uint64_t count:{uint64_t(0),uint64_t(1000),uint64_t(10000)}) {
    const std::string group="233.252.14.7";const int port=24031;
    Logger logger("snapshot_receiver.log");McastSocket rx(logger);
    Measure::require(rx.init(group,"lo",port,true)>=0&&rx.join(group),"snapshot subscription failed");
    int requested=4*1024*1024;setsockopt(rx.socket_fd_,SOL_SOCKET,SO_RCVBUF,&requested,sizeof(requested));int actual=0;socklen_t len=sizeof(actual);getsockopt(rx.socket_fd_,SOL_SOCKET,SO_RCVBUF,&actual,&len);
    MDPMarketUpdateLFQueue input(16);
    auto synth=std::make_unique<SnapshotSynthesizer>(&input,"lo",group,port);
    std::vector<MEMarketUpdate> expected;
    for(uint64_t i=1;i<=count;++i) {
      MEMarketUpdate event{MarketUpdateType::ADD,i,TickerId(i%8),Side::BUY,100+Price(256*(i%64)),1,i};
      expected.push_back(event);MDPMarketUpdate update{i,event};synth->addToSnapshot(&update);
    }
    std::vector<uint64_t> publish,recover,receive;uint64_t frames_total=0,packets_total=0;
    for(int round=0;round<5;++round) {
      std::vector<MDPMarketUpdate> frames;frames.reserve(count+10);
      std::atomic<bool> ready{false};bool fault=false;uint64_t packets=0,received_at=0;
      rx.recv_callback_=[&](McastSocket* socket) {
        ++packets;if(socket->receive_fault_||socket->next_rcv_valid_index_%Wire::MarketSize){fault=true;return;}
        for(size_t offset=0;offset<socket->next_rcv_valid_index_;offset+=Wire::MarketSize) {
          MDPMarketUpdate frame;fault|=!Wire::decode(reinterpret_cast<const uint8_t*>(socket->inbound_data_.data()+offset),frame);frames.push_back(frame);
        }
      };
      std::thread consumer([&] {
        pin(1);ready=true;const auto deadline=Measure::now()+10*NANOS_TO_SECS;
        while(frames.size()<count+10&&Measure::now()<deadline) {rx.sendAndRecv();}
        received_at=Measure::now();
      });
      pin(0);while(!ready){}const auto start=Measure::now();synth->publishSnapshot();publish.push_back(Measure::now()-start);
      consumer.join();receive.push_back(received_at-start);
      Measure::require(!fault&&frames.size()==count+10,"incomplete/corrupt snapshot UDP cycle");
      for(size_t i=0;i<frames.size();++i)Measure::require(frames[i].seq_num_==i&&frames[i].watermark_==count&&frames[i].snapshot_cycle_==frames.front().snapshot_cycle_,"snapshot seq/cycle/watermark mismatch");
      const auto a=Measure::now();Trading::MarketRecovery recovery(count+64);
      for(const auto& f:frames)recovery.ingest(true,f);
      auto batch=recovery.build();Measure::require(bool(batch)&&batch->watermark==count&&batch->hash==bookHash(expected)&&batch->events.size()==count+8,"snapshot reconstructed state mismatch");
      recovery.commit(*batch);Measure::require(!recovery.recovering()&&recovery.expected()==count+1,"snapshot commit mismatch");
      recover.push_back(Measure::now()-a);frames_total+=frames.size();packets_total+=packets;
    }
    const auto name="snapshot_live_"+std::to_string(count);uint64_t elapsed=0;for(auto n:publish)elapsed+=n;
    Measure::output(name+"_publish",publish,elapsed,frames_total);
    Measure::output(name+"_received",receive,elapsed,frames_total);
    elapsed=0;for(auto n:recover)elapsed+=n;Measure::output(name+"_rebuild_commit",recover,elapsed,frames_total);
    std::cout<<"{\"control\":\""<<name<<"\",\"frames_per_cycle\":"<<count+10<<",\"packets_total\":"<<packets_total
      <<",\"receive_buffer_bytes\":"<<actual<<",\"state_hash_ok\":true,\"watermark\":"<<count<<",\"receiver_truncation\":"<<rx.truncated_datagrams_<<"}\n";
  }
}
int main(int argc,char** argv) {
  try {
    Measure::require(argc==2,"provide queue/tcp/snapshot");Measure::clockOverhead();
    const std::string mode=argv[1];if(mode=="queue")queueProbe();else if(mode=="tcp")tcpProbe();else if(mode=="snapshot")snapshotProbe();else throw std::runtime_error("unknown component");
    return 0;
  }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
