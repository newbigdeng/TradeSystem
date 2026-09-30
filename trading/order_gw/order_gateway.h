#pragma once
#include "common/order_protocol.h"

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

  }

    /// Start and stop the order gateway main thread.
    auto start() {
      run_ = true;
      ASSERT(tcp_socket_.connect(ip_, iface_, port_, false) >= 0,
             "Unable to connect to ip:" + ip_ + " port:" + std::to_string(port_) + " on iface:" + iface_ + " error:" + std::string(std::strerror(errno)));
      worker_.reset(Common::createAndStartThread(-1, "Trading/OrderGateway", [this]() { run(); }));
    }

    auto stop() -> void {
      stop_deadline_=Common::getMonotonicNanos()+5*NANOS_TO_SECS;run_=false;
      if(worker_ && worker_->joinable())worker_->join();
    }

    /// Deleted default, copy & move constructors and assignment-operators.
    OrderGateway() = delete;

    OrderGateway(const OrderGateway &) = delete;

    OrderGateway(const OrderGateway &&) = delete;

    OrderGateway &operator=(const OrderGateway &) = delete;

    OrderGateway &operator=(const OrderGateway &&) = delete;

    std::atomic<bool>& sessionHealthy() noexcept {return session_healthy_;}
    uint64_t sessionEpoch() const noexcept {return session_epoch_;}
  private:
    const ClientId client_id_;
    const uint64_t session_epoch_=Common::getCurrentNanos();
    std::atomic<bool> session_healthy_{true};

    /// Exchange's order server's TCP server address.
    std::string ip_;
    const std::string iface_;
    const int port_ = 0;

    /// Lock free queue on which we consume client requests from the trade engine and forward them to the exchange's order server.
    Exchange::ClientRequestLFQueue *outgoing_requests_ = nullptr;

    /// Lock free queue on which we write client responses which we read and processed from the exchange, to be consumed by the trade engine.
    Exchange::ClientResponseLFQueue *incoming_responses_ = nullptr;

    std::atomic<bool> run_{false};
    std::unique_ptr<std::thread> worker_;

    std::string time_str_;
    Logger logger_;

    /// Sequence numbers to track the sequence number to set on outgoing client requests and expected on incoming client responses.
    size_t next_outgoing_seq_num_ = 1;
    size_t next_exp_seq_num_ = 1;

    /// TCP connection to the exchange's order server.
    Common::TCPSocket tcp_socket_;
    uint64_t pending_requests_=0,last_receive_=0;
    std::atomic<Nanos> stop_deadline_{INT64_MAX};

  private:
    /// Main thread loop - sends out client requests to the exchange and reads and dispatches incoming client responses.
    auto run() noexcept -> void;

    /// Callback when an incoming client response is read, we perform some checks and forward it to the lock free queue connected to the trade engine.
    auto recvCallback(TCPSocket *socket, Nanos rx_time) noexcept -> void;
  };
}
