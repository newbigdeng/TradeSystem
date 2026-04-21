#pragma once

#include <functional>

#include "socket_utils.h"
#include "logging.h"

namespace Common {
  /// TCP缓冲区大小，单位字节
  constexpr size_t TCPBufferSize = 64 * 1024 * 1024;

  struct TCPSocket {
    explicit TCPSocket(Logger &logger)
        : logger_(logger) {
      outbound_data_.resize(TCPBufferSize);
      inbound_data_.resize(TCPBufferSize);
    }

    /// 连接到指定IP和端口
    auto connect(const std::string &ip, const std::string &iface, int port, bool is_listening) -> int;

    /// 发送和接收数据，接收数据时调用回调函数
    auto sendAndRecv() noexcept -> bool;

    /// 发送数据到发送缓冲区
    auto send(const void *data, size_t len) noexcept -> void;

    TCPSocket() = delete;
    TCPSocket(const TCPSocket &) = delete;
    TCPSocket(const TCPSocket &&) = delete;
    TCPSocket &operator=(const TCPSocket &) = delete;
    TCPSocket &operator=(const TCPSocket &&) = delete;
    /// 套接字文件描述符
    int socket_fd_ = -1;

    /// 发送和接收缓冲区
    std::vector<char> outbound_data_;
    size_t next_send_valid_index_ = 0;
    std::vector<char> inbound_data_;
    size_t next_rcv_valid_index_ = 0;

    /// 套接字属性
    struct sockaddr_in socket_attrib_{};

    /// 回调函数
    std::function<void(TCPSocket *s, Nanos rx_time)> recv_callback_ = nullptr;

    std::string time_str_;
    Logger &logger_;
  };
}
