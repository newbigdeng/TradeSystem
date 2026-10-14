// 组播收发封装。1400 字节是发送数据报上限，65536 字节是缓冲容量，两者不同。
// 发送出口与接收加入组均使用 iface_；UDP 本身不提供可靠投递保证。

#pragma once
#include <functional>
#include "socket_utils.h"
#include "logging.h"

namespace Common {
constexpr size_t McastBufferSize = 65536;  // 单个收发缓冲的容量。
constexpr size_t MaxMcastDatagram = 1400;  // 限制待发数据报长度，避免默认链路上的大报文分片。

struct McastSocket {
  // 借用日志创建组播对象。
  McastSocket(Logger& logger)
      // 保存日志引用。
      : logger_(logger) {
    outbound_data_.resize(McastBufferSize);  // 启动时分配发送缓冲。
    inbound_data_.resize(McastBufferSize);   // 启动时分配接收缓冲。
  }

  // 析构关闭 fd，解除该 socket 的成员关系。
  ~McastSocket() {
    if (socket_fd_ >= 0)
      ::close(socket_fd_);
  }

  McastSocket(const McastSocket&) = delete;
  McastSocket& operator=(const McastSocket&) = delete;

  // 创建发送或接收 socket。
  auto init(const std::string& ip, const std::string& iface, int port, bool is_listening) -> int;
  auto join(const std::string& ip) -> bool;                  // 在保存的接口上加入组播组。
  auto leave(const std::string& ip, int port) -> void;       // 关闭 socket 退出组播。
  auto sendAndRecv() noexcept -> bool;                       // 尝试发送待发数据报并接收一报。
  auto send(const void* data, size_t len) noexcept -> bool;  // 缓冲新帧，必要时先刷新现有数据报。
  int socket_fd_ = -1;                                       // 内核 socket 描述符，-1 表示无效。
  std::string iface_;                                        // 收发组播使用的接口名。
  std::vector<char> outbound_data_;                          // 待发送数据报存储。
  size_t next_send_valid_index_ = 0;                         // 待发送数据报的有效长度。
  std::vector<char> inbound_data_;                           // 本次接收数据报存储。
  size_t next_rcv_valid_index_ = 0;                          // 本次接收有效长度，不跨数据报累积。
  std::function<void(McastSocket* s)> recv_callback_ = nullptr;  // 解析数据报的上层回调。
  std::string time_str_;                                         // 日志时间格式化缓冲。
  Logger& logger_;                                               // 借用的日志对象。
  bool receive_fault_ = false;        // 本次数据报是否截断，供恢复层判断。
  uint64_t truncated_datagrams_ = 0,  // 累计截断报文数。
      send_retries_ = 0,              // 暂时不能发送时的重试次数。
      sent_bytes_ = 0,                // 累计送入内核的字节数。
      received_bytes_ = 0;            // 累计正常接收字节数。
};
}
