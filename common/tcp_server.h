#pragma once

#include "tcp_socket.h"

namespace Common {
  struct TCPServer {
    explicit TCPServer(Logger &logger)
        : listener_socket_(logger), logger_(logger) {
    }

    /// 监听指定接口和端口的连接
    auto listen(const std::string &iface, int port) -> void;

    /// 检查是否有新的连接或死连接并更新套接字容器
    auto poll() noexcept -> void;

    /// 发送和接收数据
    auto sendAndRecv() noexcept -> void;

  private:
    /// 添加到EPOLL列表中
    auto addToEpollList(TCPSocket *socket);
    /// 从EPOLL列表中移除
    auto deleteFromEpollList(TCPSocket *socket);
    /// 删除套接字
    auto del(TCPSocket* socket);

  public:
    /// 监听套接字文件描述符
    int epoll_fd_ = -1;
    TCPSocket listener_socket_;

    epoll_event events_[1024];// EPOLL事件数组

    /// 接收数据套接字容器
    std::vector<TCPSocket *> receive_sockets_;
    /// 发送数据套接字容器
    std::vector<TCPSocket *> receive_sockets_, send_sockets_;

    /// 接收数据回调函数
    std::function<void(TCPSocket *s, Nanos rx_time)> recv_callback_ = nullptr;
    /// 接收数据完成回调函数
    std::function<void()> recv_finished_callback_ = nullptr;

    std::string time_str_;
    Logger &logger_;
  };
}
