#pragma once
#include <mutex>
#include "common/order_protocol.h"
#include "common/book_hash.h"
#include "common/critical_journal.h"

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

    /// Start and stop the trade engine main thread.
    auto start() -> void {
      run_ = true;
      worker_.reset(Common::createAndStartThread(-1, "Trading/TradeEngine", [this] { run(); }));
    }

    auto stop() -> void {
      admitting_.store(false,std::memory_order_release);run_=false;
      if(worker_ && worker_->joinable())worker_->join();
      logger_.log("TRADE STATS accepted:% rejected:% duplicates:% queue_full:% high_watermark:%\n",accepted_requests_,rejected_requests_,duplicate_responses_,outgoing_ogw_requests_->full_count(),outgoing_ogw_requests_->high_watermark());
      logger_.log("POSITIONS\n%\n",position_keeper_.toString());
    }
    void quiesce();
    void writeCheckpoint(Common::CriticalJournal&,uint64_t epoch,bool healthy);

    /// Main loop for this thread - processes incoming client responses and market data updates which in turn may generate client requests.
    auto run() noexcept -> void;

    /// Write a client request to the lock free queue for the order server to consume and send to the exchange.
    auto sendClientRequest(const Exchange::MEClientRequest *client_request) noexcept -> bool;

    /// Process changes to the order book - updates the position keeper, feature engine and informs the trading algorithm about the update.
    auto onOrderBookUpdate(TickerId ticker_id, Price price, Side side, MarketOrderBook *book) noexcept -> void;

    /// Process trade events - updates the  feature engine and informs the trading algorithm about the trade event.
    auto onTradeUpdate(const Exchange::MEMarketUpdate *market_update, MarketOrderBook *book) noexcept -> void;

    /// Process client responses - updates the position keeper and informs the trading algorithm about the response.
    auto onOrderUpdate(const Exchange::MEClientResponse *client_response) noexcept -> void;

    /// Function wrappers to dispatch order book updates, trade events and client responses to the trading algorithm.
    std::function<void(TickerId ticker_id, Price price, Side side, MarketOrderBook *book)> algoOnOrderBookUpdate_;
    std::function<void(const Exchange::MEMarketUpdate *market_update, MarketOrderBook *book)> algoOnTradeUpdate_;
    std::function<void(const Exchange::MEClientResponse *client_response)> algoOnOrderUpdate_;

    auto initLastEventTime() {
      last_event_time_ = Common::getCurrentNanos();
    }

    auto silentSeconds() {
      return (Common::getCurrentNanos() - last_event_time_.load()) / NANOS_TO_SECS;
    }

    void setMarketTrusted(bool trusted) noexcept {market_trusted_.store(trusted,std::memory_order_release);}
    std::atomic<bool>& marketTrust() noexcept {return market_trusted_;}
    std::atomic<uint64_t>& marketGeneration() noexcept {return market_generation_;}
    void setOrderSession(std::atomic<bool>* state) noexcept {order_session_=state;}
    const PositionInfo* positionInfo(TickerId ticker) const {return position_keeper_.getPositionInfo(ticker);}
    auto clientId() const {
      return client_id_;
    }

    /// Deleted default, copy & move constructors and assignment-operators.
    TradeEngine() = delete;

    TradeEngine(const TradeEngine &) = delete;

    TradeEngine(const TradeEngine &&) = delete;

    TradeEngine &operator=(const TradeEngine &) = delete;

    TradeEngine &operator=(const TradeEngine &&) = delete;

  private:
    /// This trade engine's ClientId.
    const ClientId client_id_;

    /// Hash map container from TickerId -> MarketOrderBook.
    MarketOrderBookHashMap ticker_order_book_;

    /// Lock free queues.
    /// One to publish outgoing client requests to be consumed by the order gateway and sent to the exchange.
    /// Second to consume incoming client responses from, written to by the order gateway based on data received from the exchange.
    /// Third to consume incoming market data updates from, written to by the market data consumer based on data received from the exchange.
    Exchange::ClientRequestLFQueue *outgoing_ogw_requests_ = nullptr;
    Exchange::ClientResponseLFQueue *incoming_ogw_responses_ = nullptr;
    Exchange::MEMarketUpdateLFQueue *incoming_md_updates_ = nullptr;

    std::atomic<Nanos> last_event_time_{0};
    std::atomic<bool> run_{false};
    std::unique_ptr<std::thread> worker_;
    std::mutex state_mutex_;
    std::atomic<bool> admitting_{true};
    std::atomic<bool> market_trusted_{false};
    std::atomic<uint64_t> market_generation_{0};
    std::atomic<bool>* order_session_=nullptr;
    bool reconciled_=true;
    uint64_t last_response_id_=0,accepted_requests_=0,rejected_requests_=0,duplicate_responses_=0;

    std::string time_str_;
    Logger logger_;

    /// Feature engine for the trading algorithms.
    FeatureEngine feature_engine_;

    /// Position keeper to track position, pnl and volume.
    PositionKeeper position_keeper_;

    /// Order manager to simplify the task of managing orders for the trading algorithms.
    OrderManager order_manager_;

    /// Risk manager to track and perform pre-trade risk checks.
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
