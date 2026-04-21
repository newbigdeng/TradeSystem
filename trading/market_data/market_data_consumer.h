#pragma once

#include <functional>
#include <map>

#include "common/thread_utils.h"
#include "common/lf_queue.h"
#include "common/macros.h"
#include "common/mcast_socket.h"

#include "exchange/market_data/market_update.h"

namespace Trading {
  class MarketDataConsumer {
  public:
    MarketDataConsumer(Common::ClientId client_id, Exchange::MEMarketUpdateLFQueue *market_updates, const std::string &iface,
                       const std::string &snapshot_ip, int snapshot_port,
                       const std::string &incremental_ip, int incremental_port);

    ~MarketDataConsumer() {
      stop();

      using namespace std::literals::chrono_literals;
      std::this_thread::sleep_for(5s);
    }

    /// 启动和停止市场数据消费者主线程。
    auto start() {
      run_ = true;
      ASSERT(Common::createAndStartThread(-1, "Trading/MarketDataConsumer", [this]() { run(); }) != nullptr, "Failed to start MarketData thread.");
    }

    auto stop() -> void {
      run_ = false;
    }

    /// 删除默认、复制和移动构造函数及赋值操作符。
    MarketDataConsumer() = delete;
    MarketDataConsumer(const MarketDataConsumer &) = delete;
    MarketDataConsumer(const MarketDataConsumer &&) = delete;
    MarketDataConsumer &operator=(const MarketDataConsumer &) = delete;
    MarketDataConsumer &operator=(const MarketDataConsumer &&) = delete;
  private:
    /// 跟踪增量市场数据流中的下一个预期序列号，用于检测间隔/丢包。
    size_t next_exp_inc_seq_num_ = 1;

    /// 解码后的市场数据更新推送至的无锁队列，由交易引擎消费。
    Exchange::MEMarketUpdateLFQueue *incoming_md_updates_ = nullptr;

    volatile bool run_ = false;

    std::string time_str_;
    Logger logger_;

    /// 增量和市场数据流的多播订阅套接字。
    Common::McastSocket incremental_mcast_socket_, snapshot_mcast_socket_;

    /// 跟踪我们是否正在进行快照市场数据流的恢复/同步，可能是因为我们刚刚启动或丢包了。
    bool in_recovery_ = false;

    /// 快照多播流的信息。
    const std::string iface_, snapshot_ip_;
    const int snapshot_port_;

    /// 用于从快照和增量通道排队市场数据更新的容器，按序列号递增顺序排队。
    typedef std::map<size_t, Exchange::MEMarketUpdate> QueuedMarketUpdates;
    QueuedMarketUpdates snapshot_queued_msgs_, incremental_queued_msgs_;

  private:
    /// 此线程的主循环 - 从多播套接字读取和处理消息 - 主要工作在recvCallback()和checkSnapshotSync()方法中。
    auto run() noexcept -> void;

    /// 处理市场数据更新，消费者需要使用套接字参数来确定这是来自快照还是增量流。
    auto recvCallback(McastSocket *socket) noexcept -> void;

    /// 在*_queued_msgs_容器中排队消息，第一个参数指定此更新是来自快照还是增量流。
    auto queueMessage(bool is_snapshot, const Exchange::MDPMarketUpdate *request);

    /// 通过订阅快照多播流来启动快照同步过程。
    auto startSnapshotSync() -> void;

    /// 检查是否可以从快照和增量市场数据流排队的市场数据更新中进行恢复/同步。
    auto checkSnapshotSync() -> void;
  };
}