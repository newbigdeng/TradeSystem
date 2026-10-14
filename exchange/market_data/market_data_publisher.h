#pragma once

#include <functional>

#include "market_data/snapshot_synthesizer.h"

namespace Exchange {
class MarketDataPublisher {
 public:
  MarketDataPublisher(MEMarketUpdateLFQueue* market_updates, const std::string& iface,
                      const std::string& snapshot_ip, int snapshot_port,
                      const std::string& incremental_ip, int incremental_port);

  ~MarketDataPublisher() {
    stop();

    delete snapshot_synthesizer_;
    snapshot_synthesizer_ = nullptr;
  }

  // 启动或停止行情发布线程及其快照合成线程。

  auto start() {
    run_ = true;

    worker_.reset(
        Common::createAndStartThread(-1, "Exchange/MarketDataPublisher", [this]() { run(); }));

    snapshot_synthesizer_->start();
  }

  auto stop() -> void {
    run_ = false;
    if (worker_ && worker_->joinable())
      worker_->join();

    snapshot_synthesizer_->stop();
  }

  // 从撮合队列取行情，发布增量组播，再交给快照合成器。

  auto run() noexcept -> void;

  // 禁止默认构造、复制和移动，避免重复拥有资源。
  MarketDataPublisher() = delete;
  MarketDataPublisher(const MarketDataPublisher&) = delete;
  MarketDataPublisher(const MarketDataPublisher&&) = delete;
  MarketDataPublisher& operator=(const MarketDataPublisher&) = delete;
  MarketDataPublisher& operator=(const MarketDataPublisher&&) = delete;

 private:
  /// 增量行情的下一条序号。
  size_t next_inc_seq_num_ = 1;

  /// 接收撮合引擎行情的无锁队列。
  MEMarketUpdateLFQueue* outgoing_md_updates_ = nullptr;

  // 向快照合成器转发增量行情的无锁队列。

  MDPMarketUpdateLFQueue snapshot_md_updates_;

  std::atomic<bool> run_{false};
  std::unique_ptr<std::thread> worker_;

  std::string time_str_;
  Logger logger_;

  /// 增量行情的组播 socket。
  Common::McastSocket incremental_socket_;

  // 定期合成并发布完整订单簿快照。

  SnapshotSynthesizer* snapshot_synthesizer_ = nullptr;
};
}
