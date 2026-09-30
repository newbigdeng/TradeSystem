#include "snapshot_synthesizer.h"
#include "common/book_hash.h"
#include "common/market_protocol.h"

namespace Exchange {
  SnapshotSynthesizer::SnapshotSynthesizer(MDPMarketUpdateLFQueue *market_updates, const std::string &iface,
                                           const std::string &snapshot_ip, int snapshot_port)
      : snapshot_md_updates_(market_updates), logger_("exchange_snapshot_synthesizer.log"), snapshot_socket_(logger_), order_pool_(ME_MAX_ORDER_IDS) {
    ASSERT(snapshot_socket_.init(snapshot_ip, iface, snapshot_port, /*is_listening*/ false) >= 0,
           "Unable to create snapshot mcast socket. error:" + std::string(std::strerror(errno)));
    for(auto& orders : ticker_orders_)
      orders.fill(nullptr);
  }

  SnapshotSynthesizer::~SnapshotSynthesizer() {
    stop();
  }

  /// Start and stop the snapshot synthesizer thread.
  void SnapshotSynthesizer::start() {
    run_ = true;
    worker_.reset(Common::createAndStartThread(-1, "Exchange/SnapshotSynthesizer", [this]() { run(); }));
  }

  void SnapshotSynthesizer::stop() {
    run_=false;
    if(worker_ && worker_->joinable())worker_->join();
  }

  /// Process an incremental market update and update the limit order book snapshot.
  auto SnapshotSynthesizer::addToSnapshot(const MDPMarketUpdate *market_update) {
    const auto &me_market_update = market_update->me_market_update_;
    ASSERT(me_market_update.ticker_id_<ME_MAX_TICKERS,"invalid ticker in snapshot input");
    if(me_market_update.type_==MarketUpdateType::ADD || me_market_update.type_==MarketUpdateType::MODIFY || me_market_update.type_==MarketUpdateType::CANCEL)ASSERT(me_market_update.order_id_>0 && me_market_update.order_id_<ME_MAX_ORDER_IDS,"invalid market order ID");
    auto *orders = &ticker_orders_.at(me_market_update.ticker_id_);
    switch (me_market_update.type_) {
      case MarketUpdateType::ADD: {
        auto order = orders->at(me_market_update.order_id_);
        ASSERT(order == nullptr, "Received:" + me_market_update.toString() + " but order already exists:" + (order ? order->toString() : ""));
        orders->at(me_market_update.order_id_) = order_pool_.allocate(me_market_update);
        ASSERT(orders->at(me_market_update.order_id_)!=nullptr,"snapshot pool exhausted");
      }
        break;
      case MarketUpdateType::MODIFY: {
        auto order = orders->at(me_market_update.order_id_);
        ASSERT(order != nullptr, "Received:" + me_market_update.toString() + " but order does not exist.");
        ASSERT(order->order_id_ == me_market_update.order_id_, "Expecting existing order to match new one.");
        ASSERT(order->side_ == me_market_update.side_, "Expecting existing order to match new one.");

        order->qty_ = me_market_update.qty_;
        order->price_ = me_market_update.price_;
      }
        break;
      case MarketUpdateType::CANCEL: {
        auto order = orders->at(me_market_update.order_id_);
        ASSERT(order != nullptr, "Received:" + me_market_update.toString() + " but order does not exist.");
        ASSERT(order->order_id_ == me_market_update.order_id_, "Expecting existing order to match new one.");
        ASSERT(order->side_ == me_market_update.side_, "Expecting existing order to match new one.");

        order_pool_.deallocate(order);
        orders->at(me_market_update.order_id_) = nullptr;
      }
        break;
      case MarketUpdateType::RECOVERY_COMMIT:
      case MarketUpdateType::SNAPSHOT_START:
      case MarketUpdateType::CLEAR:
      case MarketUpdateType::SNAPSHOT_END:
      case MarketUpdateType::TRADE:
      case MarketUpdateType::INVALID:
        break;
    }

    ASSERT(market_update->seq_num_ == last_inc_seq_num_ + 1, "Expected incremental seq_nums to increase.");
    last_inc_seq_num_ = market_update->seq_num_;
  }

  /// Publish a full snapshot cycle on the snapshot multicast stream.
  auto SnapshotSynthesizer::publishSnapshot() {
    std::vector<MEMarketUpdate> live;
    for(const auto& orders:ticker_orders_)for(const auto* order:orders)if(order)live.push_back(*order);
    const auto hash=Common::bookHash(live);const auto cycle=++snapshot_cycle_;uint64_t sequence=0;
    auto publish=[&](const MEMarketUpdate& event) {
      const auto bytes=Common::Wire::encode(MDPMarketUpdate{sequence++,event,cycle,last_inc_seq_num_,hash});
      ASSERT(snapshot_socket_.send(bytes.data(),bytes.size()),"snapshot UDP output congested; fail closed instead of publishing an incomplete cycle");
      snapshot_socket_.sendAndRecv();
    };
    publish({MarketUpdateType::SNAPSHOT_START,last_inc_seq_num_});
    for(TickerId ticker=0;ticker<ME_MAX_TICKERS;++ticker) {
      publish({MarketUpdateType::CLEAR,OrderId_INVALID,ticker});
      for(const auto* order:ticker_orders_[ticker])if(order)publish(*order);
    }
    publish({MarketUpdateType::SNAPSHOT_END,last_inc_seq_num_});snapshot_socket_.sendAndRecv();
    logger_.log("SNAPSHOT cycle:% watermark:% hash:% orders:%\n",cycle,last_inc_seq_num_,hash,live.size());
  }

  /// Main method for this thread - processes incremental updates from the market data publisher, updates the snapshot and publishes the snapshot periodically.
  void SnapshotSynthesizer::run() {
    logger_.log("%:% %() %\n", __FILE__, __LINE__, __FUNCTION__, getCurrentTimeStr(&time_str_));
    const auto* configured=std::getenv("TRADE_SNAPSHOT_MS");
    const auto milliseconds=configured?std::strtoll(configured,nullptr,10):60000;
    ASSERT(milliseconds>0 && milliseconds<=3600000,"TRADE_SNAPSHOT_MS out of bounds");
    const auto period=milliseconds*NANOS_TO_MILLIS;
    while(run_ || snapshot_md_updates_->peek()) {
      for (auto market_update = snapshot_md_updates_->peek(); snapshot_md_updates_->size() && market_update; market_update = snapshot_md_updates_->peek()) {
        logger_.log("%:% %() % Processing %\n", __FILE__, __LINE__, __FUNCTION__, getCurrentTimeStr(&time_str_),
                    market_update->toString().c_str());

        addToSnapshot(market_update);

        snapshot_md_updates_->pop();
      }

      if (getCurrentNanos() - last_snapshot_time_ > period) {
        last_snapshot_time_ = getCurrentNanos();
        publishSnapshot();
      }
    }
    publishSnapshot(); // stable final watermark after the publisher has joined
  }
}
