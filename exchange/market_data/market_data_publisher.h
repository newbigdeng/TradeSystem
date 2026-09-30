#pragma once

#include <functional>

#include "market_data/snapshot_synthesizer.h"

namespace Exchange {
  class MarketDataPublisher {
  public:
    MarketDataPublisher(MEMarketUpdateLFQueue *market_updates, const std::string &iface,
                        const std::string &snapshot_ip, int snapshot_port,
                        const std::string &incremental_ip, int incremental_port);

    ~MarketDataPublisher() {
      stop();

  delete snapshot_synthesizer_;
      snapshot_synthesizer_ = nullptr;
    }

    /// Start and stop the market data publisher main thread, as well as the internal snapshot synthesizer thread.
    auto start() {
      run_ = true;

      worker_.reset(Common::createAndStartThread(-1, "Exchange/MarketDataPublisher", [this]() { run(); }));

      snapshot_synthesizer_->start();
    }

    auto stop() -> void {
      run_=false;
      if(worker_ && worker_->joinable())worker_->join();

      snapshot_synthesizer_->stop();
    }

    /// Main run loop for this thread - consumes market updates from the lock free queue from the matching engine, publishes them on the incremental multicast stream and forwards them to the snapshot synthesizer.
    auto run() noexcept -> void;

    // Deleted default, copy & move constructors and assignment-operators.
    MarketDataPublisher() = delete;

    MarketDataPublisher(const MarketDataPublisher &) = delete;

    MarketDataPublisher(const MarketDataPublisher &&) = delete;

    MarketDataPublisher &operator=(const MarketDataPublisher &) = delete;

    MarketDataPublisher &operator=(const MarketDataPublisher &&) = delete;

  private:
    /// Sequencer number tracker on the incremental market data stream.
    size_t next_inc_seq_num_ = 1;

    /// Lock free queue from which we consume market data updates sent by the matching engine.
    MEMarketUpdateLFQueue *outgoing_md_updates_ = nullptr;

    /// Lock free queue on which we forward the incremental market data updates to send to the snapshot synthesizer.
    MDPMarketUpdateLFQueue snapshot_md_updates_;

    std::atomic<bool> run_{false};
    std::unique_ptr<std::thread> worker_;

    std::string time_str_;
    Logger logger_;

    /// Multicast socket to represent the incremental market data stream.
    Common::McastSocket incremental_socket_;

    /// Snapshot synthesizer which synthesizes and publishes limit order book snapshots on the snapshot multicast stream.
    SnapshotSynthesizer *snapshot_synthesizer_ = nullptr;
  };
}
