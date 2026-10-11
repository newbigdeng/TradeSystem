#include <array>
#include <cstring>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <vector>
#include "common/tcp_socket.h"
#include "exchange/order_server/order_server.h"

void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}

void flush_test() {
  Common::Logger logger("network_flush_test.log");
  int pair[2];require(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0,"socketpair");
  require(Common::setNonBlocking(pair[0])&&Common::setNonBlocking(pair[1]),"nonblocking");
  int size=4096;require(setsockopt(pair[0],SOL_SOCKET,SO_SNDBUF,&size,sizeof(size))==0,"small send buffer");
  Common::TCPSocket sender(logger,512*1024);sender.socket_fd_=pair[0];
  size_t callbacks=0;sender.recv_callback_=[&](auto*,auto){++callbacks;};
  const std::array<char,4> inbound={'p','i','n','g'};
  require(::send(pair[1],inbound.data(),inbound.size(),MSG_NOSIGNAL)==4,"peer input");
  std::vector<char> expected(512*1024),actual(expected.size());
  for(size_t i=0;i<expected.size();++i)expected[i]=char(i%251);
  require(sender.send(expected.data(),expected.size())==Common::SendResult::Accepted,"enqueue payload");
  require(sender.flushSend()&&sender.pending_bytes()>0&&sender.send_would_block_>0,"did not retain short write/EAGAIN tail");
  size_t received=0;
  for(size_t tries=0;tries<100000&&received<actual.size();++tries) {
    require(sender.flushSend(),"flush unexpectedly failed");
    const auto n=recv(pair[1],actual.data()+received,std::min<size_t>(4096,actual.size()-received),MSG_DONTWAIT);
    if(n>0)received+=size_t(n);else require(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR),"receive error");
  }
  require(actual==expected&&received==actual.size()&&sender.pending_bytes()==0,"payload lost, repeated, or incomplete");
  require(callbacks==0&&sender.next_rcv_valid_index_==0,"send-only path read input");
  std::array<char,4> peek{};
  require(recv(pair[0],peek.data(),peek.size(),MSG_PEEK|MSG_DONTWAIT)==4&&peek==inbound,"peer input was consumed");
  close(pair[1]);
  require(sender.send("x",1)==Common::SendResult::Accepted&&!sender.flushSend()&&!sender.healthy(),"closed peer not detected");
  std::cout<<"PASS send-only progress, exact short-write retry, EAGAIN retention, no receive callback, peer failure\n";
}

#ifdef EXPECT_RECEIVE_BUDGET
void idle_test() {
  Exchange::ClientRequestLFQueue requests(1);Exchange::ClientResponseLFQueue responses(1);
  {
    Exchange::OrderServer server(&requests,&responses,"lo",0);
    Common::Logger logger("network_idle_test.log");Common::TCPSocket socket(logger,4096);
    for(int i=0;i<20;++i)server.recvFinishedCallback();
    Exchange::MEClientRequest sentinel{};sentinel.order_id_=99;require(requests.try_push(sentinel),"prefill request queue");
    const Exchange::MEClientRequest request{Exchange::ClientRequestType::NEW,1,0,1,Common::Side::BUY,100,1};
    const auto bytes=Common::Wire::encode(Exchange::OMClientRequest{1,request,77});
    std::memcpy(socket.inbound_data_.data(),bytes.data(),bytes.size());socket.next_rcv_valid_index_=bytes.size();
    server.beginReceiveTurn();server.recvCallback(&socket,100);
    server.recvFinishedCallback();server.recvFinishedCallback();
    Exchange::MEClientRequest value;require(requests.try_pop(value)&&value.order_id_==99,"pending retry overwrote full queue");
    server.recvFinishedCallback();require(requests.try_pop(value)&&value.order_id_==1,"pending request was skipped");
    for(int i=0;i<20;++i)server.recvFinishedCallback();
  }
  std::ifstream input("exchange_order_server.log");std::string line;size_t timings=0;
  while(std::getline(input,line))timings+=line.find("RDTSC Exchange_FIFOSequencer_sequenceAndPublish ")!=std::string::npos;
#if TRADE_PERF_TRACE_EVERY == 1
  require(timings==3,"idle callback still emits timings or pending retries disappeared");
#else
  require(timings<=3,"idle timing flood");
#endif
  std::cout<<"PASS idle timing suppression and nonempty backpressure retries\n";
}

void budget_test() {
  Exchange::ClientRequestLFQueue requests(32);Exchange::ClientResponseLFQueue responses(32);
  Exchange::OrderServer server(&requests,&responses,"lo",0);
  Common::Logger logger("network_budget_test.log");
  Common::TCPSocket first(logger,4096),second(logger,4096);
  auto prepare=[](auto& socket,Common::ClientId cid) {
    for(uint64_t i=1;i<=12;++i) {
      Exchange::MEClientRequest request{Exchange::ClientRequestType::NEW,cid,0,i,Common::Side::BUY,100,1};
      const auto bytes=Common::Wire::encode(Exchange::OMClientRequest{i,request,77});
      std::memcpy(socket.inbound_data_.data()+socket.next_rcv_valid_index_,bytes.data(),bytes.size());
      socket.next_rcv_valid_index_+=bytes.size();
    }
  };
  prepare(first,1);prepare(second,2);
  for(uint64_t turn=0;turn<3;++turn) {
    server.beginReceiveTurn();
    server.recvCallback(&first,100);server.recvCallback(&first,100);
    server.recvCallback(&second,100);server.recvCallback(&second,100);
    require(first.next_rcv_valid_index_==(8-4*turn)*Common::Wire::RequestSize,"same connection exceeded budget");
    require(second.next_rcv_valid_index_==first.next_rcv_valid_index_,"one connection starved another");
    server.recvFinishedCallback();
    for(Common::ClientId cid:{1,2})for(uint64_t j=1;j<=4;++j) {
      Exchange::MEClientRequest value;
      require(requests.try_pop(value)&&value.client_id_==cid&&value.order_id_==turn*4+j&&value.request_seq_==turn*4+j,"budget reordered or lost requests");
    }
    require(!requests.peek(),"unexpected duplicate request");
  }
  // A frame split across calls remains buffered until its final byte arrives.
  Exchange::MEClientRequest next{Exchange::ClientRequestType::NEW,1,0,13,Common::Side::BUY,100,1};
  const auto bytes=Common::Wire::encode(Exchange::OMClientRequest{13,next,77});
  std::memcpy(first.inbound_data_.data(),bytes.data(),bytes.size());first.next_rcv_valid_index_=bytes.size()-1;
  server.beginReceiveTurn();server.recvCallback(&first,101);server.recvFinishedCallback();
  require(!requests.peek()&&first.next_rcv_valid_index_==bytes.size()-1,"partial frame admitted");
  ++first.next_rcv_valid_index_;server.recvCallback(&first,101);server.recvFinishedCallback();
  Exchange::MEClientRequest value;require(requests.try_pop(value)&&value.order_id_==13&&!requests.peek(),"partial frame completion failed");
  std::cout<<"PASS per-turn per-connection admission bound, buffered retries, FIFO sequences, partial frames\n";
}
#endif

int main(int argc,char** argv) {
  const std::string mode=argc>1?argv[1]:"flush";
  if(mode=="flush")flush_test();
#ifdef EXPECT_RECEIVE_BUDGET
  else if(mode=="budget")budget_test();
  else if(mode=="idle")idle_test();
#endif
  else throw std::invalid_argument("unknown test");
}
