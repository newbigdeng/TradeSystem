#pragma once

#include <functional>

#include "common/thread_utils.h"
#include "common/macros.h"
#include "common/tcp_server.h"

#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"

namespace Trading {
  class OrderGateway {
  public:
    OrderGateway(ClientId client_id,
                 Exchange::ClientRequestLFQueue *client_requests,
                 Exchange::ClientResponseLFQueue *client_responses,
                 std::string ip, const std::string &iface, int port);

    ~OrderGateway() {
      stop();

      using namespace std::literals::chrono_literals;
      std::this_thread::sleep_for(5s);
    }

    /// 启动和停止订单网关主线程。
    auto start() {
      run_ = true;
      ASSERT(tcp_socket_.connect(ip_, iface_, port_, false) >= 0,
             "Unable to connect to ip:" + ip_ + " port:" + std::to_string(port_) + " on iface:" + iface_ + " error:" + std::string(std::strerror(errno)));
      ASSERT(Common::createAndStartThread(-1, "Trading/OrderGateway", [this]() { run(); }) != nullptr, "Failed to start OrderGateway thread.");
    }

    auto stop() -> void {
      run_ = false;
    }

    /// 删除默认、复制和移动构造函数及赋值操作符。
    OrderGateway() = delete;

    OrderGateway(const OrderGateway &) = delete;

    OrderGateway(const OrderGateway &&) = delete;

    OrderGateway &operator=(const OrderGateway &) = delete;

    OrderGateway &operator=(const OrderGateway &&) = delete;

  private:
    const ClientId client_id_;

    /// 交易所订单服务器的TCP服务器地址。
    std::string ip_;
    const std::string iface_;
    const int port_ = 0;

    /// 我们从中消费交易引擎的客户端请求并将其转发到交易所订单服务器的无锁队列。
    Exchange::ClientRequestLFQueue *outgoing_requests_ = nullptr;

    /// 我们将从交易所读取和处理的客户端响应写入的无锁队列，供交易引擎消费。
    Exchange::ClientResponseLFQueue *incoming_responses_ = nullptr;

    volatile bool run_ = false;

    std::string time_str_;
    Logger logger_;

    /// 序列号，用于跟踪要设置在传出客户端请求上的序列号和在传入客户端响应上期望的序列号。
    size_t next_outgoing_seq_num_ = 1;
    size_t next_exp_seq_num_ = 1;

    /// 到交易所订单服务器的TCP连接。
    Common::TCPSocket tcp_socket_;

  private:
    /// 主线程循环 - 向交易所发送客户端请求并读取和分发传入的客户端响应。
    auto run() noexcept -> void;

    /// 读取传入客户端响应时的回调，我们执行一些检查并将其转发到连接到交易引擎的无锁队列。
    auto recvCallback(TCPSocket *socket, Nanos rx_time) noexcept -> void;
  };
}