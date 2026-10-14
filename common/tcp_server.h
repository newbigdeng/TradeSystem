// TCP 监听器管理接入连接并分发收包回调；由 OrderServer 工作线程调用。
// 析构必须发生在网络线程退出以后，避免回调访问已经释放的 socket。

#pragma once
#include "tcp_socket.h"
#include "types.h"

namespace Common {
struct TCPServer {
  // 借用日志构造监听器。
  explicit TCPServer(Logger& logger)
      // 监听 socket 与服务器共用日志。
      : listener_socket_(logger), logger_(logger) {}

  // 调用方先停网络线程，再析构资源。
  ~TCPServer() {
    for (auto* socket : receive_sockets_)
      delete socket;  // 释放接入连接对象，并关闭其 fd。
    if (epoll_fd_ >= 0)
      ::close(epoll_fd_);  // 关闭 epoll 实例。
  }

  // 汇总未发送字节，供停机排空判断。
  size_t pendingBytes() const noexcept {
    size_t pending = 0;

    // 逐连接累计待发长度。
    for (const auto* socket : receive_sockets_)
      pending += socket->pending_bytes();
    return pending;
  }

  // 尽快刷新本轮新生成的回报，不进入接收路径。
  void flushSend() noexcept {
    for (auto* socket : receive_sockets_)  // 遍历已登记连接。
      if (socket->pending_bytes())
        socket->flushSend();  // 仅对有积压的连接执行有限预算发送。
  }

  // ip 为空时沿用接口地址，否则绑定明确指定的本机地址。
  auto listen(const std::string& iface, int port, const std::string& ip = "") -> void;
  auto poll() noexcept -> void;         // 查询就绪事件并接入新连接。
  auto sendAndRecv() noexcept -> void;  // 处理已登记连接收发，并调用轮末回调。

 private:
  auto addToEpollList(TCPSocket* socket);  // 向 epoll 登记 socket 指针与事件。

 public:
  int epoll_fd_ = -1;          // epoll 文件描述符。
  TCPSocket listener_socket_;  // 负责 accept 的监听 socket。
  epoll_event events_[1024];   // 固定长度的就绪事件数组。

  // 登记接入连接和可写事件列表；当前 I/O 遍历接收列表。
  std::vector<TCPSocket*> receive_sockets_, send_sockets_;

  // 每次收到或重试缓冲字节时调用。
  std::function<void(TCPSocket* s, Nanos rx_time)> recv_callback_ = nullptr;
  std::function<void()> recv_finished_callback_ = nullptr;  // 每轮结束调用，允许定序器重试积压。
  std::string time_str_;                                    // 日志时间格式化缓冲。
  Logger& logger_;                                          // 借用日志对象。
};
}
