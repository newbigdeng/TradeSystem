// TCP 监听器管理接入连接并分发收包回调；由 OrderServer 工作线程调用。
// 析构必须发生在网络线程退出以后，避免回调访问已经释放的 socket。

#pragma once // 防止头文件重复包含。
#include "tcp_socket.h" // 引入tcp_socket.h 的声明。
#include "types.h" // 引入types.h 的声明。
namespace Common { // 所属命名空间。
  struct TCPServer { // 定义TCPServer 的接口和状态。
    explicit TCPServer(Logger &logger) // 借用日志构造监听器。
        : listener_socket_(logger), logger_(logger) { // 监听 socket 与服务器共用日志。
    } // 结束当前作用域。

    ~TCPServer() { // 调用方先停网络线程，再析构资源。
      for (auto *socket : receive_sockets_) delete socket; // 释放接入连接对象，并关闭其 fd。
      if (epoll_fd_ >= 0) ::close(epoll_fd_); // 关闭 epoll 实例。
    } // 结束当前作用域。

    size_t pendingBytes() const noexcept { // 汇总未发送字节，供停机排空判断。
      size_t pending = 0; // 从零开始累加。
      for (const auto *socket : receive_sockets_) pending += socket->pending_bytes(); // 逐连接累计待发长度。
      return pending; // 返回总积压字节数。
    } // 结束当前作用域。

    void flushSend() noexcept { // 尽快刷新本轮新生成的回报，不进入接收路径。
      for (auto *socket : receive_sockets_) // 遍历已登记连接。
        if (socket->pending_bytes()) socket->flushSend(); // 仅对有积压的连接执行有限预算发送。
    } // 结束发送刷新。

    auto listen(const std::string &iface, int port) -> void; // 绑定接口和端口并启动监听。
    auto poll() noexcept -> void; // 查询就绪事件并接入新连接。
    auto sendAndRecv() noexcept -> void; // 处理已登记连接收发，并调用轮末回调。
   private: // 内部状态与辅助接口。
    auto addToEpollList(TCPSocket *socket); // 向 epoll 登记 socket 指针与事件。
   public: // 对外接口。
    int epoll_fd_ = -1; // epoll 文件描述符。
    TCPSocket listener_socket_; // 负责 accept 的监听 socket。
    epoll_event events_[1024]; // 固定长度的就绪事件数组。
    std::vector<TCPSocket *> receive_sockets_, send_sockets_; // 登记接入连接和可写事件列表；当前 I/O 遍历接收列表。
    std::function<void(TCPSocket *s, Nanos rx_time)> recv_callback_ = nullptr; // 每次收到或重试缓冲字节时调用。
    std::function<void()> recv_finished_callback_ = nullptr; // 每轮结束调用，允许定序器重试积压。
    std::string time_str_; // 日志时间格式化缓冲。
    Logger &logger_; // 借用日志对象。
  }; // 结束当前作用域。
} // 结束当前作用域。
