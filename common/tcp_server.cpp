// 监听 socket 使用 EPOLLET；accept 循环取连接，直到返回失败。
// 已接入连接保存在 receive_sockets_，每轮均调用 sendAndRecv，不只依赖新的 epoll 边沿。
// recv_finished_callback_ 每轮调用，让定序器在没有新收包时也能重试积压。
// 当前没有移除终止连接，长期反复断连会消耗连接名额；本次未改变连接回收策略。

#include "tcp_server.h"

namespace Common {
// 把 socket 登记到 epoll。
auto TCPServer::addToEpollList(TCPSocket* socket) {
  // 订阅边沿触发的可读事件，并保存对象指针。
  epoll_event ev{EPOLLET | EPOLLIN, {reinterpret_cast<void*>(socket)}};

  // 执行 EPOLL_CTL_ADD，0 表示成功。
  return !epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, socket->socket_fd_, &ev);
}

// 创建 TCP 监听器。
auto TCPServer::listen(const std::string& iface, int port, const std::string& ip) -> void {
  epoll_fd_ = epoll_create(1);  // 创建 epoll 实例。

  // 检查 epoll 创建结果。
  ASSERT(epoll_fd_ >= 0, "epoll_create() failed error:" + std::string(std::strerror(errno)));
  ASSERT(listener_socket_.connect(ip, iface, port, true) >=
             0,  // 绑定配置的 TCP 地址；空地址沿用网卡 IPv4。
         // 补充监听失败的接口和端口信息。
         "Listener socket failed to connect. iface:" + iface + " port:" + std::to_string(port) +
             " error:" + std::string(std::strerror(errno)));  // 补充系统错误原因。

  // 把监听 socket 加入 epoll。
  ASSERT(addToEpollList(&listener_socket_),
         "epoll_ctl() failed. error:" + std::string(std::strerror(errno)));
}

// 处理已登记连接的实际 I/O。
auto TCPServer::sendAndRecv() noexcept -> void {
  auto recv = false;  // 汇总本轮是否收到字节。

  // 每轮遍历连接，不只处理新边沿。
  std::for_each(receive_sockets_.begin(), receive_sockets_.end(), [&recv](auto socket) {
    recv |= socket->sendAndRecv();  // 执行该连接的有限预算收发。
  });
  (void)recv;  // 当前不使用汇总结果，显式避免未使用警告。

  // 无论是否收包都调用轮末回调，重试定序器积压。
  if (recv_finished_callback_)
    recv_finished_callback_();
}

// 非阻塞查询 epoll 并接入新连接。
auto TCPServer::poll() noexcept -> void {
  // 限制事件数，不超过 events_ 数组容量。
  const int max_events = std::min<size_t>(1024, 1 + receive_sockets_.size());

  // 超时设为 0，立即返回已就绪事件。
  const int n = epoll_wait(epoll_fd_, events_, max_events, 0);
  bool have_new_connection = false;  // 记录监听 socket 是否提示新连接。
  for (int i = 0; i < n; ++i) {      // 逐个处理本轮事件。
    const auto& event = events_[i];  // 引用当前事件，避免复制。

    // 取出登记时保存的 socket 指针。
    auto socket = reinterpret_cast<TCPSocket*>(event.data.ptr);
    if (event.events & EPOLLIN) {         // 处理可读事件。
      if (socket == &listener_socket_) {  // 区分监听 fd 与已接入连接。
        logger_.log("%:% %() % EPOLLIN listener_socket:%\n", __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(), socket->socket_fd_);  // 补充日志时间和相关字段。
        have_new_connection = true;                                    // 标记稍后执行 accept 循环。
        continue;                                                      // 监听 fd 不走普通收包分支。
      }

      logger_.log("%:% %() % EPOLLIN socket:%\n", __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(), socket->socket_fd_);  // 补充日志时间和相关字段。

      // 避免重复登记同一连接。
      if (std::find(receive_sockets_.begin(), receive_sockets_.end(), socket) ==
          receive_sockets_.end())
        receive_sockets_.push_back(socket);  // 纳入每轮 I/O 遍历。
    }

    if (event.events & EPOLLOUT) {  // 处理可写事件；当前注册标志未订阅 EPOLLOUT。
      logger_.log("%:% %() % EPOLLOUT socket:%\n", __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(), socket->socket_fd_);  // 补充日志时间和相关字段。

      // 避免发送列表出现重复指针。
      if (std::find(send_sockets_.begin(), send_sockets_.end(), socket) == send_sockets_.end())
        send_sockets_.push_back(socket);  // 保存可写连接；实际发送仍在 sendAndRecv 中执行。
    }

    if (event.events & (EPOLLERR | EPOLLHUP)) {  // 处理错误或挂断事件。
      logger_.log("%:% %() % EPOLLERR socket:%\n", __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(), socket->socket_fd_);  // 补充日志时间和相关字段。

      // 确保终止连接也会经过状态检查。
      if (std::find(receive_sockets_.begin(), receive_sockets_.end(), socket) ==
          receive_sockets_.end())
        receive_sockets_.push_back(socket);  // 加入收发遍历列表。
    }
  }

  while (have_new_connection) {  // 有新连接提示时持续 accept，直到失败退出。
    logger_.log("%:% %() % have_new_connection\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr());  // 补充日志时间和相关字段。
    sockaddr_storage addr;                     // 准备对端地址缓冲。
    socklen_t addr_len = sizeof(addr);         // 向 accept 提供缓冲长度。

    // 取出一个待接入连接并获得独立 fd。
    int fd = accept(listener_socket_.socket_fd_, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (fd == -1)  // 当前对所有 accept 错误都结束本轮，包括 EAGAIN。
      break;       // 退出接入循环。

    // 达到连接名额上限时关闭新 fd。
    if (receive_sockets_.size() >= ME_MAX_NUM_CLIENTS) {
      ::close(fd);
      continue;
    }
    ASSERT(setNonBlocking(fd) && disableNagle(fd),  // 接入 fd 设置非阻塞并关闭 Nagle。
           // 设置失败时给出 fd 信息。
           "Failed to set non-blocking or no-delay on socket:" + std::to_string(fd));
    ASSERT(setSOTimestamp(fd), "cannot timestamp accepted socket");  // 为接入 fd 启用内核时间戳。
    logger_.log("%:% %() % accepted socket:%\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(), fd);  // 补充日志时间和相关字段。
    auto socket = new TCPSocket(logger_);          // 分配连接对象和固定收发缓冲。
    socket->socket_fd_ = fd;                       // 把已接入 fd 交给连接对象管理。
    socket->recv_callback_ = recv_callback_;       // 设置业务层的收包回调。

    // 把连接加入 epoll 并检查结果。
    ASSERT(addToEpollList(socket),
           "Unable to add socket. error:" + std::string(std::strerror(errno)));

    // 避免重复纳入 I/O 列表。
    if (std::find(receive_sockets_.begin(), receive_sockets_.end(), socket) ==
        receive_sockets_.end())
      receive_sockets_.push_back(socket);  // 新连接开始参与每轮收发。
  }
}
}
