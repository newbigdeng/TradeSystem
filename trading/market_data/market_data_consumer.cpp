// 分别订阅增量和快照组播。截断、解码失败、序号缺口或下游满，都使行情进入不可信状态。
// 恢复批次包含 CLEAR、存量 ADD、连续增量和 RECOVERY_COMMIT；一次准入队列，失败整批重试。
// 只有策略线程应用完整批次并核对代次/哈希后，行情才重新可信；这里发布成功不等于已应用。

#include "market_data_consumer.h" // 引入market_data_consumer.h 的声明。
namespace Trading { // 所属命名空间。
MarketDataConsumer::MarketDataConsumer(Common::ClientId client,Exchange::MEMarketUpdateLFQueue* output,const std::string& iface,const std::string& snap_ip,int snap_port,const std::string& inc_ip,int inc_port,std::atomic<bool>* trusted,std::atomic<uint64_t>* generation) // 接收输出队列、两路组播端点及共享信任状态。
  :output_(output),logger_("trading_market_data_consumer_"+std::to_string(client)+".log"),incremental_(logger_),snapshot_(logger_), // 保存输出队列，先构造日志再构造两个 socket。
   trusted_(trusted?trusted:&local_trusted_),generation_(generation?generation:&local_generation_) { // 外部未提供原子状态时使用对象内的备用状态。
  trusted_->store(false);observed_generation_=generation_->fetch_add(1)+1; // 启动时行情不可信，并开启新的恢复代次。
  ASSERT(incremental_.init(inc_ip,iface,inc_port,true)>=0 && incremental_.join(inc_ip),"cannot subscribe incremental stream"); // 绑定增量接收端口并在指定接口加入组播组。
  ASSERT(snapshot_.init(snap_ip,iface,snap_port,true)>=0 && snapshot_.join(snap_ip),"cannot subscribe snapshot stream"); // 绑定快照接收端口并在指定接口加入组播组。
  incremental_.recv_callback_=[this](auto* socket){receive(socket);};snapshot_.recv_callback_=incremental_.recv_callback_; // 两路 socket 共用解析回调，内部按对象身份区分快照。
} // 结束当前作用域。

void MarketDataConsumer::stale() { // 使当前行情失效并重新开始恢复。
  trusted_->store(false,std::memory_order_release);observed_generation_=generation_->fetch_add(1,std::memory_order_acq_rel)+1; // 先关闭信任，再推进代次，防止旧批次重新启用交易。
  recovery_.restart();candidate_.reset();logger_.log("MARKET STALE: generation:% queue_full:%\n",observed_generation_,output_->full_count()); // 清空恢复缓存和候选，记录队列背压情况。
} // 结束当前作用域。

void MarketDataConsumer::run() { // 行情接收线程循环。
  while(run_) { // 运行标志为真时持续轮询。
    if(generation_->load(std::memory_order_acquire)!=observed_generation_) { // 交易引擎可能主动要求新一轮恢复。
      trusted_->store(false);observed_generation_=generation_->load();recovery_.restart();candidate_.reset(); // 同步外部代次并丢弃旧候选。
    } // 结束当前作用域。

    incremental_.sendAndRecv();snapshot_.sendAndRecv();publishRecovery(); // 轮询两路 UDP，并重试待发布的完整恢复批次。
  } // 结束当前作用域。

  logger_.log("RECOVERY COUNTERS gaps:% duplicates:% cache_overflow:% bad_hash:% recoveries:% queue_full:% high_watermark:%\n",recovery_.gaps_,recovery_.duplicates_,recovery_.overflows_,recovery_.hash_failures_,recovery_.recoveries_,output_->full_count(),output_->high_watermark()); // 记录诊断日志。
} // 结束当前作用域。

void MarketDataConsumer::receive(Common::McastSocket* socket) { // 验证单个 UDP 数据报并逐帧解码。
  if(socket->receive_fault_ || socket->next_rcv_valid_index_%Common::Wire::MarketSize) {socket->next_rcv_valid_index_=0;stale();return;} // 截断或长度不是帧长整数倍，丢弃该报文并进入恢复。
  for(size_t offset=0;offset<socket->next_rcv_valid_index_;offset+=Common::Wire::MarketSize) { // 按固定 MarketSize 步长遍历完整帧。
    Exchange::MDPMarketUpdate update; // 保存一条解码结果。
    if(!Common::Wire::decode(reinterpret_cast<const uint8_t*>(socket->inbound_data_.data()+offset),update)) {stale();break;} // 解码失败时使行情失效并结束本报文处理。
    process(socket==&snapshot_,update); // 根据 socket 身份区分快照和增量。
  } // 结束当前作用域。

  socket->next_rcv_valid_index_=0; // 本报文处理结束，清空接收有效长度。
} // 结束当前作用域。

void MarketDataConsumer::process(bool snapshot,const Exchange::MDPMarketUpdate& update) { // 处理一条合法编码的行情帧。
  if(snapshot && !recovery_.recovering())return; // 实时状态不采用新的周期快照。
  if(!snapshot && !recovery_.recovering()) { // 实时状态下优先检查增量连续性。
    if(recovery_.liveCandidate(update)) { // 判断该序号是否为下一条连续增量。
      if(output_->try_push(update.me_market_update_)) {recovery_.commitLive();return;} // 进入下游队列后才推进实时期望序号。
      stale(); // 队列满使本地行情失效，转为快照恢复。
    } else if(!recovery_.recovering())return; // 重复旧帧直接忽略，不触发恢复。
    else {trusted_->store(false);observed_generation_=generation_->fetch_add(1)+1;candidate_.reset();} // 检测到缺口时关闭信任并建立新代次。
  } // 结束当前作用域。

  recovery_.ingest(snapshot,update);candidate_=recovery_.build(); // 缓存输入并尝试构造完整、连续、哈希正确的恢复批次。
  if(candidate_) { // 批次通过恢复校验。
    Exchange::MEMarketUpdate marker;marker.type_=Exchange::MarketUpdateType::RECOVERY_COMMIT;marker.order_id_=candidate_->watermark;marker.priority_=observed_generation_;marker.state_hash_=candidate_->hash; // 附加水位、代次与最终状态哈希组成提交标记。
    candidate_->events.push_back(marker);publishRecovery(); // 将提交标记置于批次末尾，然后尝试整批发布。
  } // 结束当前作用域。
} // 结束当前作用域。

void MarketDataConsumer::publishRecovery() { // 重试恢复批次的队列准入。
  if(!candidate_)return; // 没有候选时无需处理。
  if(output_->try_push_batch(candidate_->events)) { // 空间够才一次发布全批，失败一个元素也不写。
    logger_.log("RECOVERY PUBLISHED watermark:% hash:% generation:% events:%\n",candidate_->watermark,candidate_->hash,observed_generation_,candidate_->events.size()); // 记录诊断日志。
    recovery_.commit(*candidate_);candidate_.reset(); // 发布成功后推进恢复状态并清除候选；可信确认仍由策略线程完成。
  } else std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 空间不足退让 1ms，此时两路 socket 的轮询也会暂停。
} // 结束当前作用域。
} // 结束当前作用域。
