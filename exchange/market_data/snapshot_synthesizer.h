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
    SnapshotSynthesizer(MDPMarketUpdateLFQueue *market_updates, const std::string &iface,
                        const std::string &snapshot_ip, int snapshot_port);

    ~SnapshotSynthesizer();

    /// 启动和停止快照合成器线程。
    auto start() -> void;

    auto stop() -> void;

    /// 处理增量市场更新并更新限价订单簿快照。
    auto addToSnapshot(const MDPMarketUpdate *market_update);

    /// 在快照多播流上发布完整的快照周期。
    auto publishSnapshot();

    /// 此线程的主方法 - 处理由市场数据发布者发送的增量更新，更新快照并定期发布快照。
    auto run() -> void;

    
    SnapshotSynthesizer() = delete;
    SnapshotSynthesizer(const SnapshotSynthesizer &) = delete;
    SnapshotSynthesizer(const SnapshotSynthesizer &&) = delete;
    SnapshotSynthesizer &operator=(const SnapshotSynthesizer &) = delete;
    SnapshotSynthesizer &operator=(const SnapshotSynthesizer &&) = delete;
  private:
    /// 包含来自市场数据发布者的增量市场数据更新的无锁队列。
    MDPMarketUpdateLFQueue *snapshot_md_updates_ = nullptr;

    Logger logger_;

    volatile bool run_ = false;

    std::string time_str_;

    /// Multicast socket for the snapshot multicast stream.
    McastSocket snapshot_socket_;

    /// Hash map from TickerId -> Full limit order book snapshot containing information for every live order.
    std::array<std::array<MEMarketUpdate *, ME_MAX_ORDER_IDS>, ME_MAX_TICKERS> ticker_orders_;
    size_t last_inc_seq_num_ = 0;
    Nanos last_snapshot_time_ = 0;

    /// Memory pool to manage MEMarketUpdate messages for the orders in the snapshot limit order books.
    MemPool<MEMarketUpdate> order_pool_;
  };
}