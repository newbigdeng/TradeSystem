#include "mcast_socket.h"
namespace Common {
int McastSocket::init(const std::string& ip,const std::string& iface,int port,bool listening) {
  if(socket_fd_>=0)::close(socket_fd_);
  iface_=iface;socket_fd_=createSocket(logger_,SocketCfg{ip,iface,port,true,listening,false});return socket_fd_;
}
bool McastSocket::join(const std::string& ip){return Common::join(socket_fd_,ip,iface_);}
void McastSocket::leave(const std::string&,int){if(socket_fd_>=0)::close(socket_fd_);socket_fd_=-1;}
bool McastSocket::sendAndRecv() noexcept {
  if(socket_fd_<0)return false;
  if(next_send_valid_index_) {
    const auto n=::send(socket_fd_,outbound_data_.data(),next_send_valid_index_,MSG_DONTWAIT|MSG_NOSIGNAL);
    if(n==static_cast<ssize_t>(next_send_valid_index_)) {sent_bytes_+=n;next_send_valid_index_=0;}
    else if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR))++send_retries_;
    else FATAL("multicast send failure: "+std::string(strerror(errno)));
  }
  iovec iov{inbound_data_.data(),inbound_data_.size()};msghdr message{};message.msg_iov=&iov;message.msg_iovlen=1;
  const auto n=recvmsg(socket_fd_,&message,MSG_DONTWAIT|MSG_TRUNC);
  if(n>0) {
    receive_fault_=(message.msg_flags&MSG_TRUNC) || size_t(n)>inbound_data_.size();
    next_rcv_valid_index_=receive_fault_?0:size_t(n);
    if(receive_fault_)++truncated_datagrams_;else received_bytes_+=n;
    if(recv_callback_)recv_callback_(this);
    return true;
  }
  return false;
}
bool McastSocket::send(const void* data,size_t length) noexcept {
  if(!data || !length || length>MaxMcastDatagram)return false;
  if(next_send_valid_index_+length>MaxMcastDatagram)sendAndRecv();
  if(next_send_valid_index_+length>MaxMcastDatagram)return false;
  memcpy(outbound_data_.data()+next_send_valid_index_,data,length);next_send_valid_index_+=length;return true;
}
}
