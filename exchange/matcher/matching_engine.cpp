#include "matching_engine.h"

namespace Exchange {
  MatchingEngine::MatchingEngine(ClientRequestLFQueue *client_requests, ClientResponseLFQueue *client_responses,
                                 MEMarketUpdateLFQueue *market_updates,Common::CriticalJournal* audit)
      : incoming_requests_(client_requests), outgoing_ogw_responses_(client_responses), outgoing_md_updates_(market_updates),
        logger_("exchange_matching_engine.log"),audit_(audit) {
    for(size_t i = 0; i < ticker_order_book_.size(); ++i) {
      ticker_order_book_[i] = new MEOrderBook(i, &logger_, this);
    }
  }

  MatchingEngine::~MatchingEngine() {
    stop();

incoming_requests_ = nullptr;
    outgoing_ogw_responses_ = nullptr;
    outgoing_md_updates_ = nullptr;

    for(auto& order_book : ticker_order_book_) {
      delete order_book;
      order_book = nullptr;
    }
  }

  /// Start and stop the matching engine main thread.
  auto MatchingEngine::start() -> void {
    run_ = true;
    worker_.reset(Common::createAndStartThread(-1, "Exchange/MatchingEngine", [this]() { run(); }));
  }

  auto MatchingEngine::stop() -> void {
    run_=false;
    if(worker_ && worker_->joinable())worker_->join();
  }
}
