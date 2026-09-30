#include "tcp_socket.h"
namespace Common {
int TCPSocket::connect(const std::string& ip,const std::string& iface,int port,bool listening) {
  socket_fd_=createSocket(logger_,SocketCfg{ip,iface,port,false,listening,true});
  state_=socket_fd_>=0?ConnectionState::Open:ConnectionState::Error;
  if(socket_fd_<0) last_error_=errno;
  return socket_fd_;
}
SendResult TCPSocket::send(const void* data,size_t length) noexcept {
  if(!healthy() || !data || !length) return SendResult::Invalid;
  if(length>outbound_data_.size()-next_send_valid_index_) return SendResult::Full;
  if(send_begin_+next_send_valid_index_+length>outbound_data_.size()) {
    memmove(outbound_data_.data(),outbound_data_.data()+send_begin_,next_send_valid_index_); send_begin_=0;
  }
  memcpy(outbound_data_.data()+send_begin_+next_send_valid_index_,data,length);
  next_send_valid_index_+=length; return SendResult::Accepted;
}
bool TCPSocket::sendAndRecv() noexcept {
  if(!healthy() || socket_fd_<0) return false;
  // Eight system calls per direction bound work and leave other connections time.
  for(int budget=0;budget<8 && next_send_valid_index_;++budget) {
    const auto n=::send(socket_fd_,outbound_data_.data()+send_begin_,next_send_valid_index_,MSG_DONTWAIT|MSG_NOSIGNAL);
    if(n>0) {send_begin_+=n;next_send_valid_index_-=n;sent_bytes_+=n;}
    else if(n<0 && errno==EINTR) continue;
    else if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) {++send_would_block_;break;}
    else {last_error_=n==0?EPIPE:errno;state_=ConnectionState::Error;return false;}
  }
  if(!next_send_valid_index_) send_begin_=0;
  bool received=false;
  // Retry retained complete frames even without a fresh EPOLLIN edge.
  if(next_rcv_valid_index_ && recv_callback_) recv_callback_(this,last_receive_time_);
  for(int budget=0;budget<8 && healthy();++budget) {
    if(next_rcv_valid_index_==inbound_data_.size()) break; // downstream backpressure
    char control[CMSG_SPACE(sizeof(timeval))]{};
    iovec iov{inbound_data_.data()+next_rcv_valid_index_,inbound_data_.size()-next_rcv_valid_index_};
    msghdr message{}; message.msg_iov=&iov;message.msg_iovlen=1;
    message.msg_control=control;message.msg_controllen=sizeof(control);
    const auto n=recvmsg(socket_fd_,&message,MSG_DONTWAIT);
    if(n>0) {
      next_rcv_valid_index_+=n;received_bytes_+=n;received=true;last_receive_time_=getCurrentNanos();
      for(auto *c=CMSG_FIRSTHDR(&message);c;c=CMSG_NXTHDR(&message,c)) {
        if(c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_TIMESTAMP && c->cmsg_len>=CMSG_LEN(sizeof(timeval))) {
          timeval timestamp{};memcpy(&timestamp,CMSG_DATA(c),sizeof(timestamp));
          last_receive_time_=timestamp.tv_sec*NANOS_TO_SECS+timestamp.tv_usec*NANOS_TO_MICROS;
        }
      }
      if(recv_callback_) recv_callback_(this,last_receive_time_);
    } else if(n==0) {state_=ConnectionState::PeerClosed;break;}
    else if(errno==EINTR) continue;
    else if(errno==EAGAIN || errno==EWOULDBLOCK) break;
    else {last_error_=errno;state_=ConnectionState::Error;break;}
  }
  if(!healthy()) logger_.log("TCP terminal state:% errno:% unsent_bytes:% buffered_receive:%\n",int(state_),last_error_,pending_bytes(),next_rcv_valid_index_);
  return received;
}
}
