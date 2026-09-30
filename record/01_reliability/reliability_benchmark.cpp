#include <chrono>
#include <iostream>
#include <stdexcept>
#include <sys/socket.h>
#include "common/lf_queue.h"
#include "common/mem_pool.h"
#include "common/tcp_socket.h"
#include "exchange/order_server/fifo_sequencer.h"
using Clock=std::chrono::steady_clock;
template<class T> void push(Common::LFQueue<T>& q,const T& v) {
#ifdef TRADE_BASELINE
  *q.getNextToWriteTo()=v; q.updateWriteIndex();
#else
  if(!q.try_push(v)) throw std::runtime_error("benchmark queue full");
#endif
}
template<class T> T pop(Common::LFQueue<T>& q) {
#ifdef TRADE_BASELINE
  auto *p=q.getNextToRead(); if(!p) throw std::runtime_error("empty queue");
  auto value=*p; q.updateReadIndex(); return value;
#else
  T value; if(!q.try_pop(value)) throw std::runtime_error("empty queue"); return value;
#endif
}
void result(const char* mode,uint64_t count,Clock::time_point begin,uint64_t checksum) {
  const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-begin).count();
  std::cout<<"{\"mode\":\""<<mode<<"\",\"operations\":"<<count
           <<",\"elapsed_ns\":"<<ns<<",\"ns_per_operation\":"<<double(ns)/count
           <<",\"checksum\":"<<checksum<<"}\n";
}
int main(int argc,char** argv) {
  if(argc!=2) return 2;
  const std::string mode=argv[1];
  constexpr uint64_t count=2000000;
  if(mode=="queue") {
    Common::LFQueue<uint64_t> q(1024); uint64_t sum=0;
    const auto begin=Clock::now();
    for(uint64_t i=0;i<count;++i) { push(q,i); sum+=pop(q); }
    if(sum!=count*(count-1)/2) return 1;
    result("queue",count,begin,sum);
  } else if(mode=="pool") {
    Common::MemPool<uint64_t> pool(1024); uint64_t sum=0;
    const auto begin=Clock::now();
    for(uint64_t i=0;i<count;++i) {auto *p=pool.allocate(i); if(!p) return 1; sum+=*p; pool.deallocate(p);}
    if(sum!=count*(count-1)/2) return 1;
    result("pool",count,begin,sum);
  } else if(mode=="fifo") {
    Common::Logger logger("benchmark_fifo.log");
    Exchange::ClientRequestLFQueue q(128); Exchange::FIFOSequencer fifo(&q,&logger);
    constexpr uint64_t batches=1000, width=64; uint64_t sum=0;
    const auto begin=Clock::now();
    for(uint64_t b=0;b<batches;++b) {
      for(uint64_t i=width;i>0;--i) {
        Exchange::MEClientRequest r{Exchange::ClientRequestType::NEW,1,0,i,Common::Side::BUY,100,1};
        fifo.addClientRequest(i,r);
      }
      fifo.sequenceAndPublish();
      for(uint64_t i=1;i<=width;++i) {const auto r=pop(q); if(r.order_id_!=i) return 1; sum+=r.order_id_;}
    }
    result("fifo",batches*width,begin,sum);
  } else if(mode=="tcp") {
    Common::Logger logger("benchmark_tcp.log"); int fd[2];
    if(socketpair(AF_UNIX,SOCK_STREAM,0,fd)!=0) return 1;
    Common::setNonBlocking(fd[0]); Common::setNonBlocking(fd[1]);
    Common::TCPSocket sender(logger); sender.socket_fd_=fd[0];
    constexpr size_t bytes=512, blocks=32768; char input[bytes],output[bytes]; uint64_t sum=0;
    for(size_t i=0;i<bytes;++i) input[i]=char(i%127);
    const auto begin=Clock::now();
    for(size_t b=0;b<blocks;++b) {
      sender.send(input,bytes); sender.sendAndRecv();
      size_t received=0;
      while(received<bytes) {auto n=recv(fd[1],output+received,bytes-received,MSG_DONTWAIT); if(n>0) received+=n; else sender.sendAndRecv();}
      if(memcmp(input,output,bytes)) return 1;
      sum+=received;
    }
    result("tcp",blocks,begin,sum); close(fd[1]);
#ifdef TRADE_BASELINE
    close(fd[0]);
#endif
  } else return 2;
}
