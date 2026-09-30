#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <sys/socket.h>
#include "common/lf_queue.h"
#include "common/mem_pool.h"
#include "common/tcp_socket.h"
#include "exchange/order_server/fifo_sequencer.h"

void check(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}
template<class T> bool push(Common::LFQueue<T> &q, const T &value) {
#ifdef TRADE_BASELINE
  *q.getNextToWriteTo() = value; q.updateWriteIndex(); return true;
#else
  return q.try_push(value);
#endif
}
template<class T> bool pop(Common::LFQueue<T> &q, T &value) {
#ifdef TRADE_BASELINE
  const auto *next=q.getNextToRead(); if (!next) return false;
  value=*next; q.updateReadIndex(); return true;
#else
  return q.try_pop(value);
#endif
}
void queueTest() {
  for (size_t n : {1, 2, 3}) {
    Common::LFQueue<uint64_t> q(n);
    for (size_t i=0; i<n; ++i) check(push(q, uint64_t(i)), "initial push failed");
    check(!push(q, uint64_t(999)), "full queue accepted and overwrote a message");
    for (size_t i=0; i<n; ++i) {
      uint64_t value; check(pop(q,value) && value==i, "FIFO changed after rejected push");
    }
    uint64_t value; check(!pop(q,value), "empty pop succeeded");
  }
#ifndef TRADE_BASELINE
  Common::LFQueue<uint64_t> q(3);
  constexpr uint64_t count=1000000;
  std::thread producer([&] {
    for(uint64_t i=0;i<count;++i) {
      while(!q.try_push(i)) std::this_thread::yield();
      if(i%997==0) std::this_thread::yield();
    }
  });
  bool correct=true;
  for(uint64_t i=0;i<count;++i) {
    uint64_t value;
    while(!q.try_pop(value)) std::this_thread::yield();
    if(value!=i) correct=false;
  }
  producer.join();
  check(correct && q.size()==0, "million-message transfer lost, duplicated or reordered data");
  check(q.full_count()>0 && q.high_watermark()<=3, "queue counters invalid");
#endif
}
void tcpTest() {
  Common::Logger logger("tcp_test.log");
  int fd[2]; check(socketpair(AF_UNIX,SOCK_STREAM,0,fd)==0,"socketpair failed");
  int small=1024; setsockopt(fd[0],SOL_SOCKET,SO_SNDBUF,&small,sizeof(small));
  Common::setNonBlocking(fd[0]); Common::setNonBlocking(fd[1]);
#ifdef TRADE_BASELINE
  Common::TCPSocket sender(logger); sender.outbound_data_.resize(65536);
#else
  Common::TCPSocket sender(logger,65536);
#endif
  sender.socket_fd_=fd[0];
  std::vector<char> input(65536), output;
  for(size_t i=0;i<input.size();++i) input[i]=char((i*17+3)%251);
#ifdef TRADE_BASELINE
  sender.send(input.data(),input.size());
#else
  check(sender.send(input.data(),input.size())==Common::SendResult::Accepted,"send enqueue failed");
  check(sender.send(input.data(),1)==Common::SendResult::Full,"full send buffer accepted more data");
#endif
  sender.sendAndRecv();
  check(sender.next_send_valid_index_>0,"partial send discarded pending bytes");
  for(int iteration=0;iteration<10000 && output.size()<input.size();++iteration) {
    char buffer[4096]; const auto n=recv(fd[1],buffer,sizeof(buffer),MSG_DONTWAIT);
    if(n>0) output.insert(output.end(),buffer,buffer+n);
    sender.sendAndRecv();
  }
  check(output==input && sender.next_send_valid_index_==0,"TCP stream differs after backpressure");
  close(fd[1]);
  sender.sendAndRecv();
#ifndef TRADE_BASELINE
  check(sender.state_==Common::ConnectionState::PeerClosed,"EOF not visible");
#else
  close(fd[0]);
#endif
}
struct Counted {
  inline static int alive=0;
  int value;
  explicit Counted(int v=0):value(v){++alive;}
  Counted(const Counted &other):value(other.value){++alive;}
  ~Counted(){--alive;}
};
void poolTest() {
  Common::MemPool<int> pool(2);
  auto *a=pool.allocate(10); auto *b=pool.allocate(20);
  check(a && b && *a==10 && *b==20,"last valid pool allocation failed");
#ifndef TRADE_BASELINE
  check(pool.allocate(30)==nullptr,"exhausted pool did not return nullptr");
#endif
  pool.deallocate(a); auto *c=pool.allocate(30);
  check(c && *c==30,"freed slot not reusable");
  pool.deallocate(b); pool.deallocate(c);
  {
    Common::MemPool<Counted> objects(2);
    auto *object=objects.allocate(7); objects.deallocate(object);
    check(Counted::alive==0,"deallocate did not destroy the object");
    objects.allocate(8);
  }
  check(Counted::alive==0,"pool destruction leaked live objects");
}
void fifoTest() {
  Common::Logger logger("fifo_test.log");
  Exchange::ClientRequestLFQueue q(64);
  Exchange::FIFOSequencer sequencer(&q,&logger);
  for(size_t i=0;i<33;++i) {
    Exchange::MEClientRequest r{Exchange::ClientRequestType::NEW,1,0,i+1,Common::Side::BUY,100,1};
    sequencer.addClientRequest(42,r);
  }
  sequencer.sequenceAndPublish();
  for(size_t i=0;i<33;++i) {
    Exchange::MEClientRequest r; check(pop(q,r) && r.order_id_==i+1,"equal timestamps reordered FIFO arrival");
  }
}
int main(int argc,char **argv) {
  try {
    check(argc==2,"provide test name");
    const std::string name=argv[1];
    if(name=="queue") queueTest();
    else if(name=="tcp") tcpTest();
    else if(name=="pool") poolTest();
    else if(name=="fifo") fifoTest();
    else throw std::runtime_error("unknown test");
    std::cout<<"PASS "<<name<<'\n'; return 0;
  } catch(const std::exception &e) { std::cerr<<"FAIL "<<e.what()<<'\n'; return 1; }
}
