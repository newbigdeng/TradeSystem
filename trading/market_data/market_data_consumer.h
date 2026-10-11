// 行情网络线程只接收并发布事件，交易引擎线程应用订单簿。
// trusted 和 generation 用原子状态隔离旧恢复批次；未提供外部状态时使用对象内的备用状态。

#pragma once // 防止头文件重复包含。
#include "common/mcast_socket.h" // 引入common/mcast_socket.h 的声明。
#include "common/market_protocol.h" // 引入common/market_protocol.h 的声明。
#include "market_recovery.h" // 引入market_recovery.h 的声明。
namespace Trading { // 所属命名空间。
class MarketDataConsumer { // 定义MarketDataConsumer 的接口和状态。
 public: // 对外接口。
  MarketDataConsumer(Common::ClientId,Exchange::MEMarketUpdateLFQueue*,const std::string&,const std::string&,int,const std::string&,int, // 配置客户、输出队列及两路组播端点。
                     std::atomic<bool>* trusted=nullptr,std::atomic<uint64_t>* generation=nullptr); // 可选共享信任状态与恢复代次。
  ~MarketDataConsumer(){stop();} // 销毁前等待接收线程停止。
  void start(){run_=true;worker_.reset(Common::createAndStartThread(-1,"Trading/MarketDataConsumer",[this]{run();}));} // 启动组播轮询线程。
  void stop(){run_=false;if(worker_&&worker_->joinable())worker_->join();} // 发布停止标志并等待线程结束。
  MarketDataConsumer(const MarketDataConsumer&)=delete;MarketDataConsumer& operator=(const MarketDataConsumer&)=delete; // 禁止复制或默认构造，避免重复管理线程和连接。
 private: // 内部状态与辅助接口。
  void run();void receive(Common::McastSocket*);void stale();void process(bool,const Exchange::MDPMarketUpdate&);void publishRecovery(); // 声明轮询、解析、失效、事件处理与批次发布接口。
  Exchange::MEMarketUpdateLFQueue* output_; // 向交易引擎发布事件的队列。
  Common::Logger logger_; // 所属日志对象，先于 socket 构造。
  Common::McastSocket incremental_,snapshot_; // 增量和快照接收 socket。
  MarketRecovery recovery_; // 快照与增量重建状态机。
  std::optional<MarketRecovery::Batch> candidate_; // 已校验但可能还未成功入队的恢复批次。
  std::atomic<bool> local_trusted_{false},run_{false}; // 备用信任标志和线程运行标志。
  std::atomic<uint64_t> local_generation_{0}; // 备用恢复代次。
  std::atomic<bool>* trusted_;std::atomic<uint64_t>* generation_;uint64_t observed_generation_=0; // 共享原子状态的指针，以及接收线程当前采用的代次。
  std::unique_ptr<std::thread> worker_; // 拥有行情接收线程。
}; // 结束当前作用域。
} // 结束当前作用域。
