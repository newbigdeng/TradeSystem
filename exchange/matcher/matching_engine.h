#pragma once

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
    MatchingEngine(ClientRequestLFQueue *client_requests,
                   ClientResponseLFQueue *client_responses,
                   MEMarketUpdateLFQueue *market_updates);

    ~MatchingEngine();

    /// 启动和停止匹配引擎主线程。
    auto start() -> void;

    auto stop() -> void;

    /// 用于处理从订单服务器发送的无锁队列中读取的客户端请求。
    auto processClientRequest(const MEClientRequest *client_request) noexcept {
      auto order_book = ticker_order_book_[client_request->ticker_id_];
      switch (client_request->type_) {
        case ClientRequestType::NEW: {
          START_MEASURE(Exchange_MEOrderBook_add);
          order_book->add(client_request->client_id_, client_request->order_id_, client_request->ticker_id_,
                           client_request->side_, client_request->price_, client_request->qty_);
          END_MEASURE(Exchange_MEOrderBook_add, logger_);
        }
          break;

        case ClientRequestType::CANCEL: {
          START_MEASURE(Exchange_MEOrderBook_cancel);
          order_book->cancel(client_request->client_id_, client_request->order_id_, client_request->ticker_id_);
          END_MEASURE(Exchange_MEOrderBook_cancel, logger_);
        }
          break;

        default: {
          FATAL("Received invalid client-request-type:" + clientRequestTypeToString(client_request->type_));
        }
          break;
      }
    }

    /// 将客户端响应写入无锁队列供订单服务器消费。
    auto sendClientResponse(const MEClientResponse *client_response) noexcept {
      logger_.log("%:% %() % Sending %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), client_response->toString());
      while(!outgoing_ogw_responses_->try_push(std::move(*client_response)));
      //auto next_write = outgoing_ogw_responses_->getNextToWriteTo();
      // *next_write = std::move(*client_response);
      // outgoing_ogw_responses_->updateWriteIndex();
      TTT_MEASURE(T4t_MatchingEngine_LFQueue_write, logger_);
    }

    /// 将市场数据更新写入无锁队列供市场数据发布者消费。
    auto sendMarketUpdate(const MEMarketUpdate *market_update) noexcept {
      logger_.log("%:% %() % Sending %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_), market_update->toString());
      
      while(!outgoing_md_updates_->try_push(std::move(*market_update)));
      //auto next_write = outgoing_md_updates_->getNextToWriteTo();
      // *next_write = std::move(*market_update);
      // outgoing_md_updates_->updateWriteIndex();
      TTT_MEASURE(T4_MatchingEngine_LFQueue_write, logger_);
    }

    /// 此线程的主循环 - 处理传入的客户端请求，进而生成客户端响应和市场更新。
    auto run() noexcept {
      logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
      while (run_) {
        MEClientRequest me_client_request;
        while(!incoming_requests_->try_pop(me_client_request));

        if (LIKELY(me_client_request)) {
          TTT_MEASURE(T3_MatchingEngine_LFQueue_read, logger_);

          logger_.log("%:% %() % Processing %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                      me_client_request.toString());
          START_MEASURE(Exchange_MatchingEngine_processClientRequest);
          processClientRequest(&me_client_request);
          END_MEASURE(Exchange_MatchingEngine_processClientRequest, logger_);
          //incoming_requests_->updateReadIndex();
        }
      }
    }

    /// 删除默认、拷贝和移动构造函数及赋值运算符。
    MatchingEngine() = delete;
    MatchingEngine(const MatchingEngine &) = delete;
    MatchingEngine(const MatchingEngine &&) = delete;
    MatchingEngine &operator=(const MatchingEngine &) = delete;
    MatchingEngine &operator=(const MatchingEngine &&) = delete;
  private:
    /// 从交易品种ID到匹配引擎订单簿的哈希映射容器。
    OrderBookHashMap ticker_order_book_;

    /// 无锁队列。
    /// 第一个用于消费订单服务器发送的传入客户端请求。
    /// 第二个用于发布传出的客户端响应供订单服务器消费。
    /// 第三个用于发布传出的市场更新供市场数据发布者消费。
    ClientRequestLFQueue *incoming_requests_ = nullptr;
    ClientResponseLFQueue *outgoing_ogw_responses_ = nullptr;
    MEMarketUpdateLFQueue *outgoing_md_updates_ = nullptr;

    volatile bool run_ = false;

    std::string time_str_;
    Logger logger_;
  };
}