// send() 只追加完整数据到用户态缓冲；Accepted 不代表对端收到。
// sendAndRecv() 处理内核 I/O：短写保留尾部，EAGAIN 留待下轮；每轮读写各有 8 次预算。
// TCP 无消息边界，回调按协议切帧并保留残帧。旧缓冲也要重试，不能只等待新的可读事件。
// 上层 TCPServer 会持续轮询已登记的连接，因此达到预算后仍会在后续轮次继续读取。

#include "tcp_socket.h"

namespace Common {
// 按参数创建 TCP 连接或监听 socket。
int TCPSocket::connect(const std::string& ip, const std::string& iface, int port, bool listening) {
  // 创建非阻塞 TCP socket，并启用接收时间戳。
  socket_fd_ = createSocket(logger_, SocketCfg{ip, iface, port, false, listening, true});

  // 根据创建结果记录连接状态。
  state_ = socket_fd_ >= 0 ? ConnectionState::Open : ConnectionState::Error;
  if (socket_fd_ < 0)
    last_error_ = errno;  // 创建失败时保存 errno。
  return socket_fd_;      // 返回 fd，由调用方检查。
}

// 尝试把完整数据追加到发送缓冲。
SendResult TCPSocket::send(const void* data, size_t length) noexcept {
  // 拒绝无效连接、空指针和零长度数据。
  if (!healthy() || !data || !length)
    return SendResult::Invalid;

  // 总剩余空间不足，返回 Full，缓冲保持不变。
  if (length > outbound_data_.size() - next_send_valid_index_)
    return SendResult::Full;

  // 空间够但尾部不连续时压紧缓冲。
  if (send_begin_ + next_send_valid_index_ + length > outbound_data_.size()) {
    // 把未发送的字节搬到开头，允许区间重叠。
    memmove(outbound_data_.data(), outbound_data_.data() + send_begin_, next_send_valid_index_);
    send_begin_ = 0;  // 更新未发送区域的起点。
  }

  // 复制本次数据到未发送区域之后。
  memcpy(outbound_data_.data() + send_begin_ + next_send_valid_index_, data, length);
  next_send_valid_index_ += length;  // 增加待发送字节数。
  return SendResult::Accepted;       // 表示完整入缓冲，并非对端已经收到。
}

// 只刷新已有发送缓冲，不接收新请求或执行业务回调。
bool TCPSocket::flushSend() noexcept {
  if (!healthy() || socket_fd_ < 0)
    return false;  // 终止连接不再执行 I/O。

  // 最多尝试 8 次发送，避免单连接独占循环。
  for (int budget = 0; budget < 8 && next_send_valid_index_; ++budget) {
    // 从未发送起点写内核；非阻塞并抑制 SIGPIPE。
    const auto n = ::send(socket_fd_, outbound_data_.data() + send_begin_, next_send_valid_index_,
                          MSG_DONTWAIT | MSG_NOSIGNAL);

    // 只推进实际发送的字节数，短写尾部留待重试。
    if (n > 0) {
      send_begin_ += n;
      next_send_valid_index_ -= n;
      sent_bytes_ += n;
    } else if (n < 0 && errno == EINTR)
      continue;  // 信号中断时重试，仍受本轮预算限制。
    else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // 内核暂时不可写，保留尾部并结束本轮发送。
      ++send_would_block_;
      break;
    } else {
      // 其他结果视为连接错误，保存原因。
      last_error_ = n == 0 ? EPIPE : errno;
      state_ = ConnectionState::Error;
      return false;
    }
  }

  if (!next_send_valid_index_)
    send_begin_ = 0;  // 全部发送完毕后复位起点。
  return healthy();   // EAGAIN 仍为健康连接；尚未发送的尾部保留。
}

// 执行一轮有限预算的非阻塞收发。
bool TCPSocket::sendAndRecv() noexcept {
  if (!flushSend())
    return false;         // 复用短写与错误处理，先发送再接收。
  bool received = false;  // 记录本轮是否收到新字节。

  // 先重试旧缓冲里的帧，解决下游背压解除后没有新事件的问题。
  if (next_rcv_valid_index_ && recv_callback_)
    recv_callback_(this, last_receive_time_);

  // 最多尝试 8 次接收，下一轮继续处理。
  for (int budget = 0; budget < 8 && healthy(); ++budget) {
    // 用户态接收缓冲满时停止读内核，形成背压。
    if (next_rcv_valid_index_ == inbound_data_.size())
      break;
    char control[CMSG_SPACE(sizeof(timeval))]{};  // 准备容纳 timeval 控制消息的缓冲。

    // 从现有有效数据尾部接收，不覆盖残帧。
    iovec iov{inbound_data_.data() + next_rcv_valid_index_,
              inbound_data_.size() - next_rcv_valid_index_};
    msghdr message{};                                            // 清零 recvmsg 参数结构。
    message.msg_iov = &iov;                                      // 指定数据缓冲。
    message.msg_iovlen = 1;                                      // 本次只有一个 iovec。
    message.msg_control = control;                               // 指定辅助数据缓冲，用于时间戳。
    message.msg_controllen = sizeof(control);                    // 填写辅助缓冲长度。
    const auto n = recvmsg(socket_fd_, &message, MSG_DONTWAIT);  // 非阻塞接收字节流及控制消息。
    if (n > 0) {                                                 // 收到有效字节。
      next_rcv_valid_index_ += n;                                // 延长接收缓冲的有效区间。
      received_bytes_ += n;                                      // 累计接收字节数。
      received = true;                                           // 标记本轮有新数据。
      last_receive_time_ = getCurrentNanos();  // 先用用户态时间兜底，随后优先采用内核时间戳。

      // 遍历接收控制消息。
      for (auto* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
        // 只读取类型与长度都正确的 SCM_TIMESTAMP。
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMP &&
            c->cmsg_len >= CMSG_LEN(sizeof(timeval))) {
          timeval timestamp{};  // 准备时间戳结构。

          // 复制控制消息，避免直接解引用可能未对齐的地址。
          memcpy(&timestamp, CMSG_DATA(c), sizeof(timestamp));

          // 把秒和微秒统一换算为纳秒。
          last_receive_time_ =
              timestamp.tv_sec * NANOS_TO_SECS + timestamp.tv_usec * NANOS_TO_MICROS;
        }
      }

      // 交给上层切帧，上层负责保留未消费的尾部。
      if (recv_callback_)
        recv_callback_(this, last_receive_time_);

      // 返回 0 表示对端关闭发送方向。
    } else if (n == 0) {
      state_ = ConnectionState::PeerClosed;
      break;  // 记录终止状态并退出接收循环。
    } else if (errno == EINTR)
      continue;  // 信号中断时继续尝试。
    else if (errno == EAGAIN || errno == EWOULDBLOCK)
      break;  // 当前没有更多数据，留待下一轮。
    else {
      // 其他接收错误使连接进入 Error 状态。
      last_error_ = errno;
      state_ = ConnectionState::Error;
      break;
    }
  }

  // 记录终止时尚未发送和处理的字节，便于对账。
  if (!healthy())
    logger_.log("TCP terminal state:% errno:% unsent_bytes:% buffered_receive:%\n", int(state_),
                last_error_, pending_bytes(), next_rcv_valid_index_);
  return received;  // 返回本轮是否接收到新字节。
}
}
