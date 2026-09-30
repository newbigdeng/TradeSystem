#include "trade_engine.h"

namespace Trading {
  TradeEngine::TradeEngine(Common::ClientId client_id,
                           AlgoType algo_type,
                           const TradeEngineCfgHashMap &ticker_cfg,
                           Exchange::ClientRequestLFQueue *client_requests,
                           Exchange::ClientResponseLFQueue *client_responses,
                           Exchange::MEMarketUpdateLFQueue *market_updates)
      : client_id_(client_id), outgoing_ogw_requests_(client_requests), incoming_ogw_responses_(client_responses),
        incoming_md_updates_(market_updates), logger_("trading_engine_" + std::to_string(client_id) + ".log"),
        feature_engine_(&logger_),
        position_keeper_(&logger_),
        order_manager_(&logger_, this, risk_manager_),
        risk_manager_(&logger_, &position_keeper_, ticker_cfg) {
    for (size_t i = 0; i < ticker_order_book_.size(); ++i) {
      ticker_order_book_[i] = new MarketOrderBook(i, &logger_);
      ticker_order_book_[i]->setTradeEngine(this);
    }

    // Initialize the function wrappers for the callbacks for order book changes, trade events and client responses.
    algoOnOrderBookUpdate_ = [this](auto ticker_id, auto price, auto side, auto book) {
      defaultAlgoOnOrderBookUpdate(ticker_id, price, side, book);
    };
    algoOnTradeUpdate_ = [this](auto market_update, auto book) { defaultAlgoOnTradeUpdate(market_update, book); };
    algoOnOrderUpdate_ = [this](auto client_response) { defaultAlgoOnOrderUpdate(client_response); };

    // Create the trading algorithm instance based on the AlgoType provided.
    // The constructor will override the callbacks above for order book changes, trade events and client responses.
    if (algo_type == AlgoType::MAKER) {
      mm_algo_ = new MarketMaker(&logger_, this, &feature_engine_, &order_manager_, ticker_cfg);
    } else if (algo_type == AlgoType::TAKER) {
      taker_algo_ = new LiquidityTaker(&logger_, this, &feature_engine_, &order_manager_, ticker_cfg);
    }

    for (TickerId i = 0; i < ticker_cfg.size(); ++i) {
      logger_.log("%:% %() % Initialized % Ticker:% %.\n", __FILE__, __LINE__, __FUNCTION__,
                  Common::getCurrentTimeStr(&time_str_),
                  algoTypeToString(algo_type), i,
                  ticker_cfg.at(i).toString());
    }
  }

  TradeEngine::~TradeEngine() {
    stop();

delete mm_algo_; mm_algo_ = nullptr;
    delete taker_algo_; taker_algo_ = nullptr;

    for (auto &order_book: ticker_order_book_) {
      delete order_book;
      order_book = nullptr;
    }

    outgoing_ogw_requests_ = nullptr;
    incoming_ogw_responses_ = nullptr;
    incoming_md_updates_ = nullptr;
  }

  /// Write a client request to the lock free queue for the order server to consume and send to the exchange.
  auto TradeEngine::sendClientRequest(const Exchange::MEClientRequest *client_request) noexcept -> bool {
    logger_.log("%:% %() % Sending %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                client_request->toString().c_str());
    std::lock_guard<std::mutex> lock(state_mutex_);
    if((!admitting_.load(std::memory_order_acquire) && client_request->type_==Exchange::ClientRequestType::NEW) || Wire::validate(*client_request)!=Exchange::RejectReason::NONE || client_request->client_id_!=client_id_ || !reconciled_ || (order_session_ && !order_session_->load(std::memory_order_acquire)) || (client_request->type_==Exchange::ClientRequestType::NEW && !market_trusted_.load(std::memory_order_acquire))) {
      ++rejected_requests_;logger_.log("ADMISSION REJECTED: invalid/stale/unreconciled/session-unknown\n");return false;
    }
    const auto risk=risk_manager_.reserve(*client_request);
    if(risk!=RiskCheckResult::ALLOWED) {++rejected_requests_;logger_.log("ADMISSION REJECTED: risk:%\n",riskCheckResultToString(risk));return false;}
    if(!outgoing_ogw_requests_->try_push(*client_request)) {risk_manager_.rollback(*client_request);++rejected_requests_;logger_.log("ADMISSION REJECTED: outgoing queue full\n");return false;}
    ++accepted_requests_;
    TTT_MEASURE(T10_TradeEngine_LFQueue_write, logger_);
    return true;
  }

  /// Main loop for this thread - processes incoming client responses and market data updates which in turn may generate client requests.
  auto TradeEngine::run() noexcept -> void {
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_));
    while(run_ || incoming_ogw_responses_->peek() || incoming_md_updates_->peek()) {
      for (auto client_response = incoming_ogw_responses_->peek(); client_response; client_response = incoming_ogw_responses_->peek()) {
        TTT_MEASURE(T9t_TradeEngine_LFQueue_read, logger_);

        logger_.log("%:% %() % Processing %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                    client_response->toString().c_str());
        onOrderUpdate(client_response);
        incoming_ogw_responses_->pop();
        last_event_time_ = Common::getCurrentNanos();
      }

      for (auto market_update = incoming_md_updates_->peek(); market_update; market_update = incoming_md_updates_->peek()) {
        TTT_MEASURE(T9_TradeEngine_LFQueue_read, logger_);

        logger_.log("%:% %() % Processing %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                    market_update->toString().c_str());
        if(market_update->type_==Exchange::MarketUpdateType::RECOVERY_COMMIT) {
          std::vector<Exchange::MEMarketUpdate> orders;
          for(const auto* book:ticker_order_book_) {const auto live=book->liveOrders();orders.insert(orders.end(),live.begin(),live.end());}
          const auto hash=Common::bookHash(orders);
          const bool verified=hash==market_update->state_hash_ && market_update->priority_==market_generation_.load(std::memory_order_acquire);
          market_trusted_.store(verified,std::memory_order_release);
          logger_.log("RECOVERY VERIFIED:% watermark:% hash:% expected:% generation:%\n",verified,market_update->order_id_,hash,market_update->state_hash_,market_update->priority_);
          if(!verified)market_generation_.fetch_add(1,std::memory_order_acq_rel);
          incoming_md_updates_->pop();continue;
        }
        ASSERT(market_update->ticker_id_ < ticker_order_book_.size(),
               "Unknown ticker-id on update:" + market_update->toString());
        if(!ticker_order_book_[market_update->ticker_id_]->onMarketUpdate(market_update)) {
          market_trusted_.store(false,std::memory_order_release);market_generation_.fetch_add(1,std::memory_order_acq_rel);
          logger_.log("MARKET STALE: invalid order-book transition\n");
        }
        incoming_md_updates_->pop();
        last_event_time_ = Common::getCurrentNanos();
      }
    }
  }

  /// Process changes to the order book - updates the position keeper, feature engine and informs the trading algorithm about the update.
  auto TradeEngine::onOrderBookUpdate(TickerId ticker_id, Price price, Side side, MarketOrderBook *book) noexcept -> void {
    logger_.log("%:% %() % ticker:% price:% side:%\n", __FILE__, __LINE__, __FUNCTION__,
                Common::getCurrentTimeStr(&time_str_), ticker_id, Common::priceToString(price).c_str(),
                Common::sideToString(side).c_str());

    auto bbo = book->getBBO();

    START_MEASURE(Trading_PositionKeeper_updateBBO);
    {std::lock_guard<std::mutex> lock(state_mutex_);position_keeper_.updateBBO(ticker_id,bbo);}
    END_MEASURE(Trading_PositionKeeper_updateBBO, logger_);

    START_MEASURE(Trading_FeatureEngine_onOrderBookUpdate);
    feature_engine_.onOrderBookUpdate(ticker_id, price, side, book);
    END_MEASURE(Trading_FeatureEngine_onOrderBookUpdate, logger_);

    START_MEASURE(Trading_TradeEngine_algoOnOrderBookUpdate_);
    if(market_trusted_.load(std::memory_order_acquire))algoOnOrderBookUpdate_(ticker_id,price,side,book);
    END_MEASURE(Trading_TradeEngine_algoOnOrderBookUpdate_, logger_);
  }

  /// Process trade events - updates the  feature engine and informs the trading algorithm about the trade event.
  auto TradeEngine::onTradeUpdate(const Exchange::MEMarketUpdate *market_update, MarketOrderBook *book) noexcept -> void {
    logger_.log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                market_update->toString().c_str());

    START_MEASURE(Trading_FeatureEngine_onTradeUpdate);
    feature_engine_.onTradeUpdate(market_update, book);
    END_MEASURE(Trading_FeatureEngine_onTradeUpdate, logger_);

    START_MEASURE(Trading_TradeEngine_algoOnTradeUpdate_);
    if(market_trusted_.load(std::memory_order_acquire))algoOnTradeUpdate_(market_update,book);
    END_MEASURE(Trading_TradeEngine_algoOnTradeUpdate_, logger_);
  }

  /// Process client responses - updates the position keeper and informs the trading algorithm about the response.
  auto TradeEngine::onOrderUpdate(const Exchange::MEClientResponse *client_response) noexcept -> void {
    logger_.log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                client_response->toString().c_str());

    {
      std::lock_guard<std::mutex> lock(state_mutex_);
      if(client_response->client_id_!=client_id_ || client_response->ticker_id_>=ME_MAX_TICKERS) {reconciled_=false;return;}
      if(client_response->response_id_) {
        if(client_response->response_id_<=last_response_id_) {++duplicate_responses_;return;}
        last_response_id_=client_response->response_id_;
      } else if(client_response->type_==Exchange::ClientResponseType::FILLED) {reconciled_=false;return;}
      if(!risk_manager_.onResponse(*client_response)) {reconciled_=false;logger_.log("RECONCILIATION REQUIRED: fill/reservation mismatch\n");return;}
      if(client_response->type_==Exchange::ClientResponseType::FILLED) position_keeper_.addFill(client_response);
      if(client_response->type_==Exchange::ClientResponseType::STATE && client_response->position_!=position_keeper_.getPositionInfo(client_response->ticker_id_)->position_) {
        reconciled_=false;logger_.log("RECONCILIATION REQUIRED: exchange/local position mismatch\n");return;
      }
    }

    START_MEASURE(Trading_TradeEngine_algoOnOrderUpdate_);
    algoOnOrderUpdate_(client_response);
    END_MEASURE(Trading_TradeEngine_algoOnOrderUpdate_, logger_);
  }
}

namespace Trading {
void TradeEngine::quiesce() {
  admitting_.store(false,std::memory_order_release);
  std::vector<Exchange::MEClientRequest> cancels;
  {std::lock_guard<std::mutex> lock(state_mutex_);cancels=risk_manager_.cancellations(client_id_);}
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  for(const auto& request:cancels) {
    while(!sendClientRequest(&request)) {
      if((order_session_ && !order_session_->load()) || std::chrono::steady_clock::now()>deadline) {logger_.log("ORDER SESSION UNKNOWN: outstanding cancellations need reconciliation\n");return;}
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
}
void TradeEngine::writeCheckpoint(Common::CriticalJournal& audit,uint64_t epoch,bool healthy) {
  audit.append("ACCOUNT "+std::to_string(client_id_)+" "+std::to_string(epoch)+" "+std::to_string(healthy));
  for(TickerId ticker=0;ticker<ME_MAX_TICKERS;++ticker) {
    const auto* position=position_keeper_.getPositionInfo(ticker);
    audit.append("POSITION "+std::to_string(ticker)+" "+std::to_string(position->position_)+" "+std::to_string(position->volume_));
  }
  for(const auto& request:risk_manager_.cancellations(client_id_))audit.request("UNRESOLVED",request);
}
}
