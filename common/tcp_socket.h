#pragma once
#include <functional>
#include <vector>
#include "socket_utils.h"
namespace Common {
constexpr size_t TCPBufferSize=1024*1024;
enum class SendResult { Accepted, Full, Invalid };
enum class ConnectionState { Connecting, Open, PeerClosed, Error };
struct TCPSocket {
  explicit TCPSocket(Logger& logger,size_t capacity=TCPBufferSize):outbound_data_(capacity),inbound_data_(capacity),logger_(logger) {
    ASSERT(capacity>0,"zero TCP capacity");
  }
  ~TCPSocket() { if(socket_fd_>=0) ::close(socket_fd_); }
  int connect(const std::string& ip,const std::string& iface,int port,bool listening);
  bool sendAndRecv() noexcept;
  SendResult send(const void* data,size_t length) noexcept;
  bool healthy() const noexcept { return state_==ConnectionState::Open; }
  size_t pending_bytes() const noexcept { return next_send_valid_index_; }
  TCPSocket(const TCPSocket&)=delete; TCPSocket& operator=(const TCPSocket&)=delete;
  int socket_fd_=-1;
  std::vector<char> outbound_data_; size_t next_send_valid_index_=0;
  std::vector<char> inbound_data_; size_t next_rcv_valid_index_=0;
  sockaddr_in socket_attrib_{};
  std::function<void(TCPSocket*,Nanos)> recv_callback_;
  std::string time_str_; Logger& logger_;
  ConnectionState state_=ConnectionState::Open;
  int last_error_=0;
  uint64_t sent_bytes_=0,received_bytes_=0,send_would_block_=0;
 private:
  size_t send_begin_=0;
  Nanos last_receive_time_=0;
};
}
