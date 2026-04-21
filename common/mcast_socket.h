#pragma once

#include <functional>

#include "socket_utils.h"

#include "logging.h"

namespace Common {
  /// 缓冲区大小，64MB
  constexpr size_t McastBufferSize = 64 * 1024 * 1024;

  struct McastSocket {
    McastSocket(Logger &logger)
        : logger_(logger) {
      outbound_data_.resize(McastBufferSize);
      inbound_data_.resize(McastBufferSize);
    }

    /// 初始化多播Socket，用于读取或发布多播流
    /// 暂时不加入多播组
    auto init(const std::string &ip, const std::string &iface, int port, bool is_listening) -> int;

    /// 加入多播组
    auto join(const std::string &ip) -> bool;

    /// 退出多播组
    auto leave(const std::string &ip, int port) -> void;

    /// 发送和接收多播数据
    auto sendAndRecv() noexcept -> bool;

    /// 复制数据到send缓冲区 - 不立即发送
    auto send(const void *data, size_t len) noexcept -> void;

    int socket_fd_ = -1;

    /// 发送数据的缓冲区，接收数据的缓冲区
    /// 通常情况下只需要一个缓冲区
    std::vector<char> outbound_data_;
    size_t next_send_valid_index_ = 0; // 下一个有效发送索引
    std::vector<char> inbound_data_;
    size_t next_rcv_valid_index_ = 0; // 下一个有效接收索引

    /// 接收数据的回调函数，当有数据接收时调用
    std::function<void(McastSocket *s)> recv_callback_ = nullptr;

    std::string time_str_;
    Logger &logger_;
  };
}
