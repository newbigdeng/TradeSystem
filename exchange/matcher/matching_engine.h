#pragma once
#include "common/critical_journal.h"
#include "common/order_protocol.h"

#include "common/thread_utils.h"
#include "common/lf_queue.h"
#include "common/macros.h"

#include "order_server/client_request.h"
#include "order_server/client_response.h"
#include "market_data/market_update.h"

#include "me_order_book.h"

namespace Exchange {
class MatchingEngine final {
 public:
  MatchingEngine(ClientRequestLFQueue* client_requests, ClientResponseLFQueue* client_responses,
                 MEMarketUpdateLFQueue* market_updates, Common::CriticalJournal* audit = nullptr);

  ~MatchingEngine();

  /// 启动或停止撮合线程。
  auto start() -> void;

  auto stop() -> void;

  /// 处理订单服务器通过无锁队列送来的客户请求。
  auto processClientRequest(const MEClientRequest* client_request) noexcept {
    if (audit_)
      audit_->request("APPLY", *client_request);
    const auto invalid = Common::Wire::validate(*client_request);
    if (invalid != RejectReason::NONE) {
      const MEClientResponse reject{ClientResponseType::REJECTED,
                                    client_request->client_id_,
                                    client_request->ticker_id_,
                                    client_request->order_id_,
                                    OrderId_INVALID,
                                    client_request->side_,
                                    client_request->price_,
                                    0,
                                    0,
                                    invalid};
      sendClientResponse(&reject);
      return;
    }
    auto order_book = ticker_order_book_[client_request->ticker_id_];
    switch (client_request->type_) {
      case ClientRequestType::NEW: {
        START_MEASURE(Exchange_MEOrderBook_add);
        order_book->add(client_request->client_id_, client_request->order_id_,
                        client_request->ticker_id_, client_request->side_, client_request->price_,
                        client_request->qty_);
        END_MEASURE(Exchange_MEOrderBook_add, logger_);
      } break;

      case ClientRequestType::CANCEL: {
        START_MEASURE(Exchange_MEOrderBook_cancel);
        order_book->cancel(client_request->client_id_, client_request->order_id_,
                           client_request->ticker_id_);
        END_MEASURE(Exchange_MEOrderBook_cancel, logger_);
      } break;

      case ClientRequestType::QUERY: {
        order_book->query(client_request->client_id_, client_request->order_id_);
      } break;
      default:
        break;
    }
  }

  /// 将客户回报写入订单服务器的消费队列。
  auto sendClientResponse(const MEClientResponse* client_response) noexcept -> void {
    logger_.log("%:% %() % Sending %\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(), client_response->toString());
    auto response = *client_response;
    response.response_id_ = next_response_id_++;
    if (response.client_id_ < ME_MAX_NUM_CLIENTS && response.ticker_id_ < ME_MAX_TICKERS) {
      auto& position = client_positions_[response.client_id_][response.ticker_id_];
      if (response.type_ == ClientResponseType::FILLED)
        position += int64_t(response.exec_qty_) * sideToValue(response.side_);
      response.position_ = position;
    }
    if (audit_)
      audit_->response(response);
    ASSERT(outgoing_ogw_responses_->try_push(response),
           "response queue full: matching fail-closed; reconciliation required");
    ++response_count_;
    TTT_MEASURE(T4t_MatchingEngine_LFQueue_write, logger_);
  }

  /// 将行情事件写入行情发布器的消费队列。
  auto sendMarketUpdate(const MEMarketUpdate* market_update) noexcept {
    logger_.log("%:% %() % Sending %\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(), market_update->toString());
    ASSERT(outgoing_md_updates_->try_push(*market_update),
           "critical queue full; stop instead of overwriting");
    TTT_MEASURE(T4_MatchingEngine_LFQueue_write, logger_);
  }

  /// 消费客户请求，执行撮合并产生回报和行情。
  auto run() noexcept {
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr());
    while (run_ || incoming_requests_->peek()) {
      const auto me_client_request = incoming_requests_->peek();
      if (LIKELY(me_client_request)) {
        TTT_MEASURE(T3_MatchingEngine_LFQueue_read, logger_);

        logger_.log("%:% %() % Processing %\n", __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(), me_client_request->toString());
        START_MEASURE(Exchange_MatchingEngine_processClientRequest);
        processClientRequest(me_client_request);
        END_MEASURE(Exchange_MatchingEngine_processClientRequest, logger_);
        incoming_requests_->pop();
      }
    }
  }

  /// 禁止默认构造、复制和移动，避免重复拥有资源。
  MatchingEngine() = delete;

  MatchingEngine(const MatchingEngine&) = delete;

  MatchingEngine(const MatchingEngine&&) = delete;

  MatchingEngine& operator=(const MatchingEngine&) = delete;

  MatchingEngine& operator=(const MatchingEngine&&) = delete;

 private:
  /// 按品种 ID 索引的撮合订单簿数组。
  OrderBookHashMap ticker_order_book_;  //品种ID作为下标，品种对应的订单簿作为值

  /// 跨线程交接请求、回报和行情的无锁队列。
  /// 消费订单服务器提交的客户请求。
  /// 发布回报，供订单服务器消费。
  /// 发布行情，供行情发布器消费。
  ClientRequestLFQueue* incoming_requests_ = nullptr;
  ClientResponseLFQueue* outgoing_ogw_responses_ = nullptr;
  MEMarketUpdateLFQueue* outgoing_md_updates_ = nullptr;

  std::atomic<bool> run_{false};
  std::unique_ptr<std::thread> worker_;
  uint64_t next_response_id_ = 1, response_count_ = 0;
  std::array<std::array<int64_t, ME_MAX_TICKERS>, ME_MAX_NUM_CLIENTS> client_positions_{};

  std::string time_str_;
  Logger logger_;
  Common::CriticalJournal* audit_ = nullptr;
};
}
