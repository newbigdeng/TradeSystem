// TCP 连接与固定大小的收发缓冲，由所属网络线程管理。
// send 返回 Accepted 仅表示完整数据已进入用户态缓冲；调用方据此释放上游槽位。
// 消费回调只移走完整处理的帧，背压或半帧留在 inbound_data_，下轮继续。

#pragma once
#include <functional>
#include <vector>
#include "socket_utils.h"

namespace Common {
constexpr size_t TCPBufferSize = 1024 * 1024;       // 收发缓冲各自的默认容量为 1MiB。
enum class SendResult { Accepted, Full, Invalid };  // 完整入缓冲、空间不足、无效状态三种结果。

// 连接生命周期状态；当前 connect 完成后直接设置 Open 或 Error。
enum class ConnectionState { Connecting, Open, PeerClosed, Error };

struct TCPSocket {
  // 允许测试使用更小缓冲。
  explicit TCPSocket(Logger& logger, size_t capacity = TCPBufferSize)
      // 一次分配收发缓冲，并借用日志对象。
      : outbound_data_(capacity), inbound_data_(capacity), logger_(logger) {
    ASSERT(capacity > 0, "zero TCP capacity");  // 拒绝零容量。
  }

  ~TCPSocket() {
    if (socket_fd_ >= 0)
      ::close(socket_fd_);
  }  // 对象销毁时关闭 fd。

  // 建立连接或监听，返回 fd。
  int connect(const std::string& ip, const std::string& iface, int port, bool listening);
  bool flushSend() noexcept;    // 只处理待发字节；不触发接收回调，短写/EAGAIN 留待重试。
  bool sendAndRecv() noexcept;  // 非阻塞处理发送缓冲和接收字节。

  // 完整追加一段数据，容量不足时不部分写入。
  SendResult send(const void* data, size_t length) noexcept;

  // 仅 Open 状态允许正常 I/O。
  bool healthy() const noexcept {
    return state_ == ConnectionState::Open;
  }

  // 查询尚未交给内核的字节数。
  size_t pending_bytes() const noexcept {
    return next_send_valid_index_;
  }

  TCPSocket(const TCPSocket&) = delete;
  TCPSocket& operator=(const TCPSocket&) = delete;
  int socket_fd_ = -1;                // 无效 fd 使用 -1 表示。
  std::vector<char> outbound_data_;   // 待发送字节存储。
  size_t next_send_valid_index_ = 0;  // 未发送字节数，不是绝对尾部下标。
  std::vector<char> inbound_data_;    // 累积接收字节，包含未处理的完整帧和半帧。
  size_t next_rcv_valid_index_ = 0;   // 接收缓冲有效字节数。
  sockaddr_in socket_attrib_{};       // 预留的 IPv4 地址属性，初始清零。

  // 业务解析回调，参数是当前 socket 和收包时间。
  std::function<void(TCPSocket*, Nanos)> recv_callback_;
  std::string time_str_;                           // 日志时间格式化缓冲。
  Logger& logger_;                                 // 借用的日志对象。
  ConnectionState state_ = ConnectionState::Open;  // 初始逻辑状态；执行 I/O 仍会检查 fd。
  int last_error_ = 0;                             // 保存最后一次系统错误。
  uint64_t sent_bytes_ = 0,                        // 累计送入内核的字节数。
      received_bytes_ = 0,                         // 累计收到的字节数。
      send_would_block_ = 0;                       // 发送因 EAGAIN 暂停的次数。

 private:
  size_t send_begin_ = 0;        // 待发送区域起点，短写后向后推进。
  Nanos last_receive_time_ = 0;  // 最近收包时间，重试旧缓冲时继续传给回调。
};
}
