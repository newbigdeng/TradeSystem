#pragma once

#include <algorithm>
#include <array>
#include "common/logging.h"
#include "common/perf_utils.h"
#include "order_server/client_request.h"

namespace Exchange {
  // 所有连接共用的暂存上限；满时由 OrderServer 保留 TCP 缓冲里的未处理帧。
  constexpr size_t ME_MAX_PENDING_REQUESTS = 1024;

  // 本对象只由 OrderServer 工作线程访问，不另起线程。
  // 每轮按接收时间排序，同时间戳保持解析顺序。SO_TIMESTAMP 的微秒时间戳
  // 在网络层换算成纳秒；一次 recvmsg 的多帧可能共享时间戳。
  // 这是当前待处理集合的顺序，不保证跨轮的严格物理到达顺序。
  class FIFOSequencer {
  public:
    // 队列和日志均为借用，调用方保证它们活到定序器停止使用之后。
    FIFOSequencer(ClientRequestLFQueue *client_requests, Logger *logger)
        : incoming_requests_(client_requests), logger_(logger) {}

    ~FIFOSequencer() {}

    // 只暂存，不投递。返回 false 时，调用方不能丢掉对应的 TCP 帧。
    auto addClientRequest(Nanos rx_time, const MEClientRequest &request) -> bool {
      if (pending_size_ >= pending_client_requests_.size()) return false;
      pending_client_requests_.at(pending_size_++) = std::move(RecvTimeClientRequest{rx_time, request});
      return true;
    }

    // 网络层每轮结束后调用，即使没有新数据，也要重试上轮未投递的请求。
    auto sequenceAndPublish() {
      if (UNLIKELY(!pending_size_)) return;

      logger_->log("%:% %() % Processing % requests.\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(), pending_size_);

      // 只排序有效区间；stable_sort 保留时间戳相同的请求的相对顺序。
      // 标准库实现可能申请临时缓冲；这里没有宣称热路径完全不分配。
      std::stable_sort(pending_client_requests_.begin(), pending_client_requests_.begin() + pending_size_);

      for (size_t i = 0; i < pending_size_; ++i) {
        const auto &client_request = pending_client_requests_.at(i);
        logger_->log("%:% %() % Writing RX:% Req:% to FIFO.\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(),
                     client_request.recv_time_, client_request.request_.toString());

        if (!incoming_requests_->try_push(client_request.request_)) {
          // 前 i 条已成功，保留 [i, pending_size_)，包括投递失败的这一条。
          // 三参数 std::move 会移动赋值。i==0 时无需搬移，也必须避免
          // result==first 违反该算法的目标起点不能位于源区间内的前置条件。
          if (i > 0) {
            std::move(pending_client_requests_.begin() + i,
                      pending_client_requests_.begin() + pending_size_,
                      pending_client_requests_.begin());
          }
          pending_size_ -= i;
          return;
        }
        TTT_MEASURE(T2_OrderServer_LFQueue_write, (*logger_));
      }
      pending_size_ = 0; // 全部已投递；旧槽内容不再属于有效区间。
    }

    // 仅所属工作线程可读。主线程通过 OrderServer::quiesced_ 原子标志等排空。
    size_t pending() const noexcept { return pending_size_; }
    bool canAccept() const noexcept { return pending_size_ < pending_client_requests_.size(); }

    FIFOSequencer() = delete;
    FIFOSequencer(const FIFOSequencer &) = delete;
    FIFOSequencer(const FIFOSequencer &&) = delete;
    FIFOSequencer &operator=(const FIFOSequencer &) = delete;
    FIFOSequencer &operator=(const FIFOSequencer &&) = delete;

  private:
    ClientRequestLFQueue *incoming_requests_ = nullptr;
    std::string time_str_;
    Logger *logger_ = nullptr;

    struct RecvTimeClientRequest {
      Nanos recv_time_ = 0;
      MEClientRequest request_;
      auto operator<(const RecvTimeClientRequest &rhs) const {
        return recv_time_ < rhs.recv_time_; // 严格小于，满足排序的严格弱序要求。
      }
    };
    std::array<RecvTimeClientRequest, ME_MAX_PENDING_REQUESTS> pending_client_requests_;
    size_t pending_size_ = 0;
  };
}
