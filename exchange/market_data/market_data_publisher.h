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

      using namespace std::literals::chrono_literals;
      std::this_thread::sleep_for(5s);

      delete snapshot_synthesizer_;
      snapshot_synthesizer_ = nullptr;
    }

    /// 启动和停止市场数据发布主进程，以及内部快照合成器进程。
    auto start() {
      run_ = true;

      ASSERT(Common::createAndStartThread(-1, "Exchange/MarketDataPublisher", [this]() { run(); }) != nullptr, "Failed to start MarketData thread.");

      snapshot_synthesizer_->start();
    }

    auto stop() -> void {
      run_ = false;

      snapshot_synthesizer_->stop();
    }

    /// 此线程的主运行循环 - 从匹配引擎的无锁队列消费市场更新，将它们发布到增量多播流并转发给快照合成器。
    auto run() noexcept -> void;

    // Deleted default, copy & move constructors and assignment-operators.
    MarketDataPublisher() = delete;

    MarketDataPublisher(const MarketDataPublisher &) = delete;

    MarketDataPublisher(const MarketDataPublisher &&) = delete;

    MarketDataPublisher &operator=(const MarketDataPublisher &) = delete;

    MarketDataPublisher &operator=(const MarketDataPublisher &&) = delete;

  private:
    /// 增量市场数据流上的序列号跟踪器。
    size_t next_inc_seq_num_ = 1;

    /// 无锁队列，我们从中消费匹配引擎发送的市场数据更新。
    MEMarketUpdateLFQueue *outgoing_md_updates_ = nullptr;

    /// 无锁队列，我们将增量市场数据更新转发到快照合成器。
    MDPMarketUpdateLFQueue snapshot_md_updates_;

    volatile bool run_ = false;

    std::string time_str_;
    Logger logger_;

    /// Multicast socket to represent the incremental market data stream.
    Common::McastSocket incremental_socket_;

    /// Snapshot synthesizer which synthesizes and publishes limit order book snapshots on the snapshot multicast stream.
    SnapshotSynthesizer *snapshot_synthesizer_ = nullptr;
  };
}