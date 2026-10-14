#pragma once

#include "common/types.h"
#include "common/thread_utils.h"
#include "common/lf_queue.h"
#include "common/macros.h"
#include "common/mcast_socket.h"
#include "common/mem_pool.h"
#include "common/logging.h"

#include "market_data/market_update.h"
#include "matcher/me_order.h"

using namespace Common;

namespace Exchange {
class SnapshotSynthesizer {
 public:
  SnapshotSynthesizer(MDPMarketUpdateLFQueue* market_updates, const std::string& iface,
                      const std::string& snapshot_ip, int snapshot_port);

  ~SnapshotSynthesizer();

  /// 启动或停止快照合成线程。
  auto start() -> void;

  auto stop() -> void;

  /// 应用增量行情，更新快照中的订单状态。
  void addToSnapshot(const MDPMarketUpdate* market_update);

  /// 向快照组播发布一个完整周期。
  void publishSnapshot();

  // 消费增量、更新存量状态，并定期发布完整快照。

  auto run() -> void;

  /// 禁止默认构造、复制和移动，避免重复拥有资源。
  SnapshotSynthesizer() = delete;

  SnapshotSynthesizer(const SnapshotSynthesizer&) = delete;

  SnapshotSynthesizer(const SnapshotSynthesizer&&) = delete;

  SnapshotSynthesizer& operator=(const SnapshotSynthesizer&) = delete;

  SnapshotSynthesizer& operator=(const SnapshotSynthesizer&&) = delete;

 private:
  // 接收行情发布器增量的无锁队列。

  MDPMarketUpdateLFQueue* snapshot_md_updates_ = nullptr;

  Logger logger_;

  std::atomic<bool> run_{false};
  std::unique_ptr<std::thread> worker_;

  std::string time_str_;

  /// 快照行情的组播 socket。
  McastSocket snapshot_socket_;

  // 按品种 ID 管理存量挂单；每个品种内按订单 ID 查找。

  std::array<std::array<MEMarketUpdate*, ME_MAX_ORDER_IDS>, ME_MAX_TICKERS> ticker_orders_;
  uint64_t last_inc_seq_num_ = 0;
  uint64_t snapshot_cycle_ = Common::getCurrentNanos();
  Nanos last_snapshot_time_ = 0;

  // 保存快照存量订单的行情对象池。

  MemPool<MEMarketUpdate> order_pool_;
};
}
