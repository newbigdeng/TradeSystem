// UDP 一次收发一个数据报。多个协议帧可以装进同一数据报，但不等待攒满。
// send() 通常先缓冲；容量不足时也会调用 sendAndRecv 尝试刷新，并非绝不触发系统调用。
// EAGAIN/EINTR 保留待发内容；内核接收成功不等于订阅方收到，丢包由上层序号和快照恢复处理。
// MSG_TRUNC 检测截断，故障时不把残缺报文交给协议解析。

#include "mcast_socket.h"

namespace Common {
// 创建组播发送或接收 socket。
int McastSocket::init(const std::string& ip, const std::string& iface, int port, bool listening) {
  if (socket_fd_ >= 0)
    ::close(socket_fd_);  // 重新初始化前关闭旧 fd。
  iface_ = iface;         // 保存接口名，后续加入组时使用。

  // 创建非阻塞 UDP socket；接收端绑定，发送端设置默认目的地址。
  socket_fd_ = createSocket(logger_, SocketCfg{ip, iface, port, true, listening, false});
  return socket_fd_;  // 返回 fd 供调用方检查。
}

// 在保存的接口上加入指定组。
bool McastSocket::join(const std::string& ip) {
  return Common::join(socket_fd_, ip, iface_);  // 调用 IP_ADD_MEMBERSHIP 工具函数。
}

// 当前退组方式是关闭整个 socket。
void McastSocket::leave(const std::string&, int) {
  if (socket_fd_ >= 0)
    ::close(socket_fd_);  // 关闭 fd，内核随之解除该 socket 的组播成员关系。
  socket_fd_ = -1;        // 标记 fd 不再有效。
}

// 尝试发送缓存数据报，再接收一个数据报。
bool McastSocket::sendAndRecv() noexcept {
  if (socket_fd_ < 0)
    return false;                // 无有效 fd 时不执行 I/O。
  if (next_send_valid_index_) {  // 存在待发内容才调用发送。
    // 一次发送整个数据报，非阻塞且不触发 SIGPIPE。
    const auto n = ::send(socket_fd_, outbound_data_.data(), next_send_valid_index_,
                          MSG_DONTWAIT | MSG_NOSIGNAL);

    // 只有完整发送成功才清空用户态缓冲。
    if (n == static_cast<ssize_t>(next_send_valid_index_)) {
      sent_bytes_ += n;            // 累计送入内核的字节数。
      next_send_valid_index_ = 0;  // 清空待发长度。
    }

    // 暂时不可写或被信号中断，计数并保留数据报。
    else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
      ++send_retries_;
    // 其他发送结果立即失败退出，不能静默丢弃。
    else
      FATAL("multicast send failure: " + std::string(strerror(errno)));
  }

  // 准备接收一个数据报，覆盖旧数据报内容。
  iovec iov{inbound_data_.data(), inbound_data_.size()};
  msghdr message{};        // 清零 recvmsg 参数。
  message.msg_iov = &iov;  // 指定接收缓冲。
  message.msg_iovlen = 1;  // 使用一个 iovec。

  // 非阻塞接收，并请求识别超长数据报。
  const auto n = recvmsg(socket_fd_, &message, MSG_DONTWAIT | MSG_TRUNC);
  if (n > 0) {  // 处理非空数据报。
    // 检测截断标志或超出缓冲的真实长度。
    receive_fault_ = (message.msg_flags & MSG_TRUNC) || size_t(n) > inbound_data_.size();
    next_rcv_valid_index_ = receive_fault_ ? 0 : size_t(n);  // 故障数据报不暴露残缺字节。
    if (receive_fault_)
      ++truncated_datagrams_;  // 累计截断次数。
    else
      received_bytes_ += n;  // 正常报文累计接收字节。
    if (recv_callback_)
      recv_callback_(this);  // 上层读取 receive_fault_ 并决定是否进入恢复。
    return true;             // 表示本轮收到数据报，不表示其内容一定有效。
  }

  return false;  // 本轮未收到非空数据报。
}

// 缓冲完整数据；空间不足时先尝试刷新。
bool McastSocket::send(const void* data, size_t length) noexcept {
  // 拒绝无效数据和超过单报文上限的输入。
  if (!data || !length || length > MaxMcastDatagram)
    return false;

  // 空间不够时尝试发送当前缓冲。
  if (next_send_valid_index_ + length > MaxMcastDatagram)
    sendAndRecv();

  // 刷新后仍放不下，返回 false 且不追加新数据。
  if (next_send_valid_index_ + length > MaxMcastDatagram)
    return false;

  // 把新帧追加到待发数据报。
  memcpy(outbound_data_.data() + next_send_valid_index_, data, length);
  next_send_valid_index_ += length;  // 增加待发长度。
  return true;                       // 新帧已完整进入缓冲。
}
}
