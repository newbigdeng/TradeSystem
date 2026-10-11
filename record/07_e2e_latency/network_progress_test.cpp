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

void cross_connection_test() {
  Common::Logger logger("network_cross_connection.log");
  Common::TCPServer server(logger);
  int a[2],b[2];
  require(socketpair(AF_UNIX,SOCK_STREAM,0,a)==0 && socketpair(AF_UNIX,SOCK_STREAM,0,b)==0,"two connections");
  for(int fd:{a[0],a[1],b[0],b[1]})require(Common::setNonBlocking(fd),"nonblocking peer");
  auto* first=new Common::TCPSocket(logger,4096);first->socket_fd_=a[0];
  auto* second=new Common::TCPSocket(logger,4096);second->socket_fd_=b[0];
  server.receive_sockets_={first,second}; // Server owns both sockets.
  size_t callbacks=0;
  first->recv_callback_=[&](auto*,auto){++callbacks;};
  require(::send(a[1],"input",5,MSG_NOSIGNAL)==5,"new input on earlier connection");
  require(second->send("reply",5)==Common::SendResult::Accepted,"queue response for later connection");
  server.flushSend();
  char bytes[5];
  require(::recv(b[1],bytes,5,MSG_DONTWAIT)==5 && std::memcmp(bytes,"reply",5)==0,"later response did not progress");
  require(callbacks==0 && ::recv(a[0],bytes,5,MSG_PEEK|MSG_DONTWAIT)==5,"flush processed unrelated new input");
  require(server.pendingBytes()==0,"response still pending");
  ::close(a[1]);::close(b[1]);
  std::cout<<"PASS later connection response flushed before earlier connection receive callback\n";
}

int main(int argc,char** argv) {
  if(argc>1 && std::string(argv[1])!="flush") throw std::invalid_argument("unknown test");
  flush_test();
  cross_connection_test();
}
