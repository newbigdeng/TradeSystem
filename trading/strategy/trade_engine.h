#pragma once
#include <mutex>
#include "common/order_protocol.h"
#include "common/critical_journal.h"

#include <memory>

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

#include "strategy.h"

namespace Trading {
class TradeEngine {
 public:
  TradeEngine(Common::ClientId client_id, AlgoType algo_type,
              const TradeEngineCfgHashMap& ticker_cfg,
              Exchange::ClientRequestLFQueue* client_requests,
              Exchange::ClientResponseLFQueue* client_responses,
              Exchange::MEMarketUpdateLFQueue* market_updates,
              std::unique_ptr<Strategy> strategy = {});

  ~TradeEngine();

  /// 启动或停止交易引擎线程。
  auto start() -> void {
    run_ = true;
    worker_.reset(Common::createAndStartThread(-1, "Trading/TradeEngine", [this] { run(); }));
  }

  auto stop() -> void {
    admitting_.store(false, std::memory_order_release);
    run_ = false;
    if (worker_ && worker_->joinable())
      worker_->join();
    logger_.log("TRADE STATS accepted:% rejected:% duplicates:% queue_full:% high_watermark:%\n",
                accepted_requests_, rejected_requests_, duplicate_responses_,
                outgoing_ogw_requests_->full_count(), outgoing_ogw_requests_->high_watermark());
    logger_.log("POSITIONS\n%\n", position_keeper_.toString());
  }

  void quiesce();
  void writeCheckpoint(Common::CriticalJournal&, uint64_t epoch, bool healthy);

  /// 消费客户回报和行情事件，驱动策略产生新请求。
  auto run() noexcept -> void;

  /// 将请求提交到网关队列，由网关发送给交易所。
  auto sendClientRequest(const Exchange::MEClientRequest* client_request) noexcept -> bool;

  /// 订单簿变化时更新持仓和特征，并通知策略。
  auto onOrderBookUpdate(TickerId ticker_id, Price price, Side side, MarketOrderBook* book) noexcept
      -> void;

  /// 成交事件更新特征，并通知策略。
  auto onTradeUpdate(const Exchange::MEMarketUpdate* market_update, MarketOrderBook* book) noexcept
      -> void;

  /// 客户回报更新持仓，并通知策略。
  auto onOrderUpdate(const Exchange::MEClientResponse* client_response) noexcept -> void;

  auto initLastEventTime() {
    last_event_time_ = Common::getCurrentNanos();
  }

  auto silentSeconds() {
    return (Common::getCurrentNanos() - last_event_time_.load()) / NANOS_TO_SECS;
  }

  // 测量程序的直接控制入口；正常运行通过恢复结束标记推进已应用代次。
  void setMarketTrusted(bool trusted) noexcept {
    applied_generation_.store(trusted ? market_generation_.load(std::memory_order_acquire) : 0,
                              std::memory_order_release);
  }

  // 只有已完整应用的代次等于当前代次，策略才能使用行情。
  // 先读已应用代次，再读当前代次；旧标记无法覆盖新的失效状态。
  bool marketReady() const noexcept {
    const auto applied = applied_generation_.load(std::memory_order_acquire);
    return applied == market_generation_.load(std::memory_order_acquire);
  }

  std::atomic<uint64_t>& marketGeneration() noexcept {
    return market_generation_;
  }

  void setOrderSession(std::atomic<bool>* state) noexcept {
    order_session_ = state;
  }

  const PositionInfo* positionInfo(TickerId ticker) const {
    return position_keeper_.getPositionInfo(ticker);
  }

  auto clientId() const {
    return client_id_;
  }

  /// 禁止默认构造、复制和移动，避免重复拥有资源。
  TradeEngine() = delete;

  TradeEngine(const TradeEngine&) = delete;

  TradeEngine(const TradeEngine&&) = delete;

  TradeEngine& operator=(const TradeEngine&) = delete;

  TradeEngine& operator=(const TradeEngine&&) = delete;

 private:
  /// 本交易引擎的客户端 ID。
  const ClientId client_id_;

  /// 按品种 ID 索引的行情订单簿数组。
  MarketOrderBookHashMap ticker_order_book_;

  /// 跨线程交接请求、回报和行情的无锁队列。
  /// 发布客户请求，供订单网关发送给交易所。
  /// 消费订单网关从交易所接收的客户回报。
  /// 消费行情接收器从交易所接收的行情事件。
  Exchange::ClientRequestLFQueue* outgoing_ogw_requests_ = nullptr;
  Exchange::ClientResponseLFQueue* incoming_ogw_responses_ = nullptr;
  Exchange::MEMarketUpdateLFQueue* incoming_md_updates_ = nullptr;

  std::atomic<Nanos> last_event_time_{0};
  std::atomic<bool> run_{false};
  std::unique_ptr<std::thread> worker_;
  std::mutex state_mutex_;
  std::atomic<bool> admitting_{true};
  std::atomic<uint64_t> market_generation_{1};  // 0 保留为尚未应用。
  std::atomic<uint64_t> applied_generation_{0};
  std::atomic<bool>* order_session_ = nullptr;
  bool reconciled_ = true;
  uint64_t last_response_id_ = 0, accepted_requests_ = 0, rejected_requests_ = 0,
           duplicate_responses_ = 0;

  std::string time_str_;
  Logger logger_;

  /// 计算策略使用的行情特征。
  FeatureEngine feature_engine_;

  /// 跟踪持仓、盈亏和成交量。
  PositionKeeper position_keeper_;

  /// 统一管理策略订单。
  OrderManager order_manager_;

  /// 执行报单前风险检查。
  RiskManager risk_manager_;

  // 策略在启动时确定；交易线程独占调用，不在运行中替换。
  std::unique_ptr<Strategy> strategy_;
};
}
