#pragma once

#include <functional>

#include "common/thread_utils.h"
#include "common/time_utils.h"
#include "common/lf_queue.h"
#include "common/macros.h"
#include "common/logging.h"

#include "exchange/order_server/client_request.h"
#include "exchange/order_server/client_response.h"
#include "exchange/market_data/market_update.h"

#include "market_order_book.h"

#include "feature_engine.h"
#include "position_keeper.h"
#include "order_manager.h"
#include "risk_manager.h"

#include "market_maker.h"
#include "liquidity_taker.h"

namespace Trading {
  class TradeEngine {
  public:
    TradeEngine(Common::ClientId client_id,
                AlgoType algo_type,
                const TradeEngineCfgHashMap &ticker_cfg,
                Exchange::ClientRequestLFQueue *client_requests,
                Exchange::ClientResponseLFQueue *client_responses,
                Exchange::MEMarketUpdateLFQueue *market_updates);

    ~TradeEngine();

    /// 启动和停止交易引擎主线程。
    auto start() -> void {
      run_ = true;
      ASSERT(Common::createAndStartThread(-1, "Trading/TradeEngine", [this] { run(); }) != nullptr, "Failed to start TradeEngine thread.");
    }

    auto stop() -> void {
      while(incoming_ogw_responses_->size() || incoming_md_updates_->size()) {
        logger_.log("%:% %() % Sleeping till all updates are consumed ogw-size:% md-size:%\n", __FILE__, __LINE__, __FUNCTION__,
                    Common::getCurrentTimeStr(&time_str_), incoming_ogw_responses_->size(), incoming_md_updates_->size());

        using namespace std::literals::chrono_literals;
        std::this_thread::sleep_for(10ms);
      }

      logger_.log("%:% %() % POSITIONS\n%\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                  position_keeper_.toString());

      run_ = false;
    }

    /// 此线程的主循环 - 处理传入的客户端响应和市场数据更新，这反过来可能会生成客户端请求。
    auto run() noexcept -> void;

    /// 将客户端请求写入无锁队列，供订单服务器消费并发送到交易所。
    auto sendClientRequest(const Exchange::MEClientRequest *client_request) noexcept -> void;

    /// 处理订单簿的变化 - 更新持仓管理器、特征引擎并告知交易算法有关更新的信息。
    auto onOrderBookUpdate(TickerId ticker_id, Price price, Side side, MarketOrderBook *book) noexcept -> void;

    /// 处理交易事件 - 更新特征引擎并告知交易算法有关交易事件的信息。
    auto onTradeUpdate(const Exchange::MEMarketUpdate *market_update, MarketOrderBook *book) noexcept -> void;

    /// 处理客户端响应 - 更新持仓管理器并告知交易算法有关响应的信息。
    auto onOrderUpdate(const Exchange::MEClientResponse *client_response) noexcept -> void;

    /// Function wrappers to dispatch order book updates, trade events and client responses to the trading algorithm.
    std::function<void(TickerId ticker_id, Price price, Side side, MarketOrderBook *book)> algoOnOrderBookUpdate_;
    std::function<void(const Exchange::MEMarketUpdate *market_update, MarketOrderBook *book)> algoOnTradeUpdate_;
    std::function<void(const Exchange::MEClientResponse *client_response)> algoOnOrderUpdate_;

    auto initLastEventTime() {
      last_event_time_ = Common::getCurrentNanos();
    }

    auto silentSeconds() {
      return (Common::getCurrentNanos() - last_event_time_) / NANOS_TO_SECS;
    }

    auto clientId() const {
      return client_id_;
    }

    /// 删除默认、复制和移动构造函数及赋值操作符。
    TradeEngine() = delete;
    TradeEngine(const TradeEngine &) = delete;
    TradeEngine(const TradeEngine &&) = delete;
    TradeEngine &operator=(const TradeEngine &) = delete;
    TradeEngine &operator=(const TradeEngine &&) = delete;
  private:
    /// This trade engine's ClientId.
    const ClientId client_id_;

    /// 从交易品种ID到市场订单簿的哈希映射容器。
    MarketOrderBookHashMap ticker_order_book_;

    /// 无锁队列。
    /// 一个用于发布传出的客户端请求，供订单网关消费并发送到交易所。
    /// 第二个用于消费传入的客户端响应，由订单网关根据从交易所接收的数据写入。
    /// 第三个用于消费传入的市场数据更新，由市场数据消费者根据从交易所接收的数据写入。
    Exchange::ClientRequestLFQueue *outgoing_ogw_requests_ = nullptr;
    Exchange::ClientResponseLFQueue *incoming_ogw_responses_ = nullptr;
    Exchange::MEMarketUpdateLFQueue *incoming_md_updates_ = nullptr;

    Nanos last_event_time_ = 0;
    volatile bool run_ = false;

    std::string time_str_;
    Logger logger_;

    /// 特征引擎，用于存储和更新交易算法需要的特征。
    FeatureEngine feature_engine_;

    /// 头寸追踪器，用于跟踪和管理交易算法的持仓。
    PositionKeeper position_keeper_;

    /// 订单管理器，根据任务来修改订单
    OrderManager order_manager_;

    /// 风控引擎，根据头寸和风险配置来判断订单是否合理
    RiskManager risk_manager_;

    /// Market making or liquidity taking algorithm instance - only one of these is created in a single trade engine instance.
    MarketMaker *mm_algo_ = nullptr;
    LiquidityTaker *taker_algo_ = nullptr;

    /// Default methods to initialize the function wrappers.
    auto defaultAlgoOnOrderBookUpdate(TickerId ticker_id, Price price, Side side, MarketOrderBook *) noexcept -> void {
      logger_.log("%:% %() % ticker:% price:% side:%\n", __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(&time_str_), ticker_id, Common::priceToString(price).c_str(),
                  Common::sideToString(side).c_str());
    }

    auto defaultAlgoOnTradeUpdate(const Exchange::MEMarketUpdate *market_update, MarketOrderBook *) noexcept -> void {
      logger_.log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                  market_update->toString().c_str());
    }

    auto defaultAlgoOnOrderUpdate(const Exchange::MEClientResponse *client_response) noexcept -> void {
      logger_.log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                  client_response->toString().c_str());
    }
  };
}