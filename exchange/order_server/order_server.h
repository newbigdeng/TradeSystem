#pragma once

#include <functional>

#include "common/thread_utils.h"
#include "common/macros.h"
#include "common/tcp_server.h"

#include "order_server/client_request.h"
#include "order_server/client_response.h"
#include "order_server/fifo_sequencer.h"

namespace Exchange {
  class OrderServer {
  public:
    OrderServer(ClientRequestLFQueue *client_requests, ClientResponseLFQueue *client_responses, const std::string &iface, int port);

    ~OrderServer();

    /// 启动和停止订单服务器主线程。
    auto start() -> void;

    auto stop() -> void;

    /// 此线程的主运行循环 - 接受新的客户端连接，从中接收客户端请求并向其发送客户端响应。
    auto run() noexcept {
      logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
      while (run_) {
        tcp_server_.poll();

        tcp_server_.sendAndRecv();
        MEClientResponse client_response;
        while(!outgoing_responses_->try_pop(client_response)){
          TTT_MEASURE(T5t_OrderServer_LFQueue_read, logger_);

          auto &next_outgoing_seq_num = cid_next_outgoing_seq_num_[client_response.client_id_];
          logger_.log("%:% %() % Processing cid:% seq:% %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                      client_response.client_id_, next_outgoing_seq_num, client_response.toString());

          ASSERT(cid_tcp_socket_[client_response.client_id_] != nullptr,
                 "Dont have a TCPSocket for ClientId:" + std::to_string(client_response.client_id_));
          START_MEASURE(Exchange_TCPSocket_send);
          cid_tcp_socket_[client_response.client_id_]->send(&next_outgoing_seq_num, sizeof(next_outgoing_seq_num));
          cid_tcp_socket_[client_response.client_id_]->send(client_response, sizeof(MEClientResponse));
          END_MEASURE(Exchange_TCPSocket_send, logger_);

          TTT_MEASURE(T6t_OrderServer_TCP_write, logger_);

          ++next_outgoing_seq_num;
        }
      }
    }

    /// 从TCP接收缓冲区读取客户端请求，检查序列间隙并将其转发到FIFO排序器。
    auto recvCallback(TCPSocket *socket, Nanos rx_time) noexcept {
      TTT_MEASURE(T1_OrderServer_TCP_read, logger_);
      logger_.log("%:% %() % Received socket:% len:% rx:%\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                  socket->socket_fd_, socket->next_rcv_valid_index_, rx_time);

      if (socket->next_rcv_valid_index_ >= sizeof(OMClientRequest)) {
        size_t i = 0;
        for (; i + sizeof(OMClientRequest) <= socket->next_rcv_valid_index_; i += sizeof(OMClientRequest)) {
          auto request = reinterpret_cast<const OMClientRequest *>(socket->inbound_data_.data() + i);
          logger_.log("%:% %() % Received %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), request->toString());

          if (UNLIKELY(cid_tcp_socket_[request->me_client_request_.client_id_] == nullptr)) { // first message from this ClientId.
            cid_tcp_socket_[request->me_client_request_.client_id_] = socket;
          }

          if (cid_tcp_socket_[request->me_client_request_.client_id_] != socket) { // TODO - change this to send a reject back to the client.
            logger_.log("%:% %() % Received ClientRequest from ClientId:% on different socket:% expected:%\n", __FILE__, __LINE__, __FUNCTION__,
                        Common::getCurrentTimeStr(&time_str_), request->me_client_request_.client_id_, socket->socket_fd_,
                        cid_tcp_socket_[request->me_client_request_.client_id_]->socket_fd_);
            continue;
          }

          auto &next_exp_seq_num = cid_next_exp_seq_num_[request->me_client_request_.client_id_];
          if (request->seq_num_ != next_exp_seq_num) { // TODO - change this to send a reject back to the client.
            logger_.log("%:% %() % Incorrect sequence number. ClientId:% SeqNum expected:% received:%\n", __FILE__, __LINE__, __FUNCTION__,
                        Common::getCurrentTimeStr(&time_str_), request->me_client_request_.client_id_, next_exp_seq_num, request->seq_num_);
            continue;
          }

          ++next_exp_seq_num;

          START_MEASURE(Exchange_FIFOSequencer_addClientRequest);
          fifo_sequencer_.addClientRequest(rx_time, request->me_client_request_);
          END_MEASURE(Exchange_FIFOSequencer_addClientRequest, logger_);
        }
        memcpy(socket->inbound_data_.data(), socket->inbound_data_.data() + i, socket->next_rcv_valid_index_ - i);
        socket->next_rcv_valid_index_ -= i;
      }
    }

    /// 结束从所有TCP连接读取传入消息，排序并将客户端请求发布到匹配引擎。
    auto recvFinishedCallback() noexcept {
      START_MEASURE(Exchange_FIFOSequencer_sequenceAndPublish);
      fifo_sequencer_.sequenceAndPublish();
      END_MEASURE(Exchange_FIFOSequencer_sequenceAndPublish, logger_);
    }

    /// 删除默认、复制和移动构造函数以及赋值操作符。
    OrderServer() = delete;

    OrderServer(const OrderServer &) = delete;

    OrderServer(const OrderServer &&) = delete;

    OrderServer &operator=(const OrderServer &) = delete;

    OrderServer &operator=(const OrderServer &&) = delete;

  private:
    const std::string iface_;
    const int port_ = 0;

    /// 发送给连接客户端的传出客户端响应的无锁队列。
    ClientResponseLFQueue *outgoing_responses_ = nullptr;

    volatile bool run_ = false;

    std::string time_str_;
    Logger logger_;

    /// 从客户端ID到传出客户端响应上要发送的下一个序列号的哈希映射。
    std::array<size_t, ME_MAX_NUM_CLIENTS> cid_next_outgoing_seq_num_;

    /// 从客户端ID到传入客户端请求上期望的下一个序列号的哈希映射。
    std::array<size_t, ME_MAX_NUM_CLIENTS> cid_next_exp_seq_num_;

    /// 从客户端ID到TCP套接字/客户端连接的哈希映射。
    std::array<Common::TCPSocket *, ME_MAX_NUM_CLIENTS> cid_tcp_socket_;

    /// 监听新客户端连接的TCP服务器实例。
    Common::TCPServer tcp_server_;

    /// FIFO排序器负责确保传入的客户端请求按照接收顺序进行处理。
    FIFOSequencer fifo_sequencer_;
  };
}