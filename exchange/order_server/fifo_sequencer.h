#pragma once

#include "common/thread_utils.h"
#include "common/macros.h"

#include "order_server/client_request.h"

namespace Exchange {
  /// 订单服务器/FIFO排序器中所有TCP连接上的未处理客户端请求消息的最大数量。
  constexpr size_t ME_MAX_PENDING_REQUESTS = 1024;

  class FIFOSequencer {
  public:
    FIFOSequencer(ClientRequestLFQueue *client_requests, Logger *logger)
        : incoming_requests_(client_requests), logger_(logger) {
    }

    ~FIFOSequencer() {
    }

    /// 将客户端请求排队，不立即处理，在调用sequenceAndPublish()时处理。
    auto addClientRequest(Nanos rx_time, const MEClientRequest &request) {
      if (pending_size_ >= pending_client_requests_.size()) {
        FATAL("Too many pending requests");
      }
      pending_client_requests_.at(pending_size_++) = std::move(RecvTimeClientRequest{rx_time, request});
    }

    /// 按接收时间升序对挂起的客户端请求进行排序，然后将它们写入无锁队列供匹配引擎消费。
    auto sequenceAndPublish() {
      if (UNLIKELY(!pending_size_))
        return;

      logger_->log("%:% %() % Processing % requests.\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), pending_size_);

      std::sort(pending_client_requests_.begin(), pending_client_requests_.begin() + pending_size_);

      for (size_t i = 0; i < pending_size_; ++i) {
        const auto &client_request = pending_client_requests_.at(i);

        logger_->log("%:% %() % Writing RX:% Req:% to FIFO.\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                     client_request.recv_time_, client_request.request_.toString());

        while(!incoming_requests_->try_push(std::move(client_request.request_)));
        
        //auto next_write = incoming_requests_->getNextToWriteTo();
        //*next_write = std::move(client_request.request_);
        //incoming_requests_->updateWriteIndex();
        TTT_MEASURE(T2_OrderServer_LFQueue_write, (*logger_));
      }

      pending_size_ = 0;
    }

    /// 删除默认、复制和移动构造函数以及赋值操作符。
    FIFOSequencer() = delete;
    FIFOSequencer(const FIFOSequencer &) = delete;
    FIFOSequencer(const FIFOSequencer &&) = delete;
    FIFOSequencer &operator=(const FIFOSequencer &) = delete;
    FIFOSequencer &operator=(const FIFOSequencer &&) = delete;
  private:
    /// 用于发布客户端请求的无锁队列，以便匹配引擎可以消费它们。
    ClientRequestLFQueue *incoming_requests_ = nullptr;

    std::string time_str_;
    Logger *logger_ = nullptr;

    /// 一个封装软件接收时间和客户端请求的结构。
    struct RecvTimeClientRequest {
      Nanos recv_time_ = 0;
      MEClientRequest request_;

      auto operator<(const RecvTimeClientRequest &rhs) const {
        return (recv_time_ < rhs.recv_time_);
      }
    };

    /// 挂起的客户端请求队列，未排序。
    std::array<RecvTimeClientRequest, ME_MAX_PENDING_REQUESTS> pending_client_requests_;
    size_t pending_size_ = 0;
  };
}