// 行情网络线程只接收并发布事件，交易引擎线程应用订单簿。
// generation 标识当前恢复代次；未提供外部状态时使用对象内的备用代次。

#pragma once
#include "common/mcast_socket.h"
#include "common/market_protocol.h"
#include "market_recovery.h"

namespace Trading {
class MarketDataConsumer {
 public:
  // 配置客户、输出队列及两路组播端点。
  MarketDataConsumer(Common::ClientId, Exchange::MEMarketUpdateLFQueue*, const std::string&,
                     const std::string&, int, const std::string&, int,
                     // 可选的共享恢复代次。
                     std::atomic<uint64_t>* generation = nullptr);

  ~MarketDataConsumer() {
    stop();
  }  // 销毁前等待接收线程停止。

  // 启动组播轮询线程。
  void start() {
    run_ = true;
    worker_.reset(
        Common::createAndStartThread(-1, "Trading/MarketDataConsumer", [this] { run(); }));
  }

  // 发布停止标志并等待线程结束。
  void stop() {
    run_ = false;
    if (worker_ && worker_->joinable())
      worker_->join();
  }

  MarketDataConsumer(const MarketDataConsumer&) = delete;
  MarketDataConsumer& operator=(const MarketDataConsumer&) = delete;

 private:
  // 声明轮询、解析、失效、事件处理与批次发布接口。
  void run();
  void receive(Common::McastSocket*);
  void stale();
  void process(bool, const Exchange::MDPMarketUpdate&);
  void publishRecovery();
  Exchange::MEMarketUpdateLFQueue* output_;         // 向交易引擎发布事件的队列。
  Common::Logger logger_;                           // 所属日志对象，先于 socket 构造。
  Common::McastSocket incremental_, snapshot_;      // 增量和快照接收 socket。
  MarketRecovery recovery_;                         // 快照与增量重建状态机。
  std::optional<MarketRecovery::Batch> candidate_;  // 已校验但可能还未成功入队的恢复批次。
  std::atomic<bool> run_{false};                    // 线程运行标志。
  std::atomic<uint64_t> local_generation_{0};       // 备用恢复代次。

  // 共享原子状态的指针，以及接收线程当前采用的代次。
  std::atomic<uint64_t>* generation_;
  uint64_t observed_generation_ = 0;
  std::unique_ptr<std::thread> worker_;  // 拥有行情接收线程。
};
}
