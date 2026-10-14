// 分别订阅增量和快照组播；正常状态忽略快照，增量异常才使当前行情失效。
// 恢复批次包含 CLEAR、存量 ADD、连续增量和 RECOVERY_COMMIT；一次准入队列，失败整批重试。
// 行情校验集中在接收线程；交易引擎应用到结束标记后记录代次，不重复计算状态哈希。

#include "market_data_consumer.h"

namespace Trading {
// 接收输出队列、两路组播端点及共享信任状态。
MarketDataConsumer::MarketDataConsumer(Common::ClientId client,
                                       Exchange::MEMarketUpdateLFQueue* output,
                                       const std::string& iface, const std::string& snap_ip,
                                       int snap_port, const std::string& inc_ip, int inc_port,
                                       std::atomic<uint64_t>* generation)
    // 保存输出队列，先构造日志再构造两个 socket。
    : output_(output),
      logger_("trading_market_data_consumer_" + std::to_string(client) + ".log"),
      incremental_(logger_),
      snapshot_(logger_),
      // 外部未提供恢复代次时使用对象内的备用状态。
      generation_(generation ? generation : &local_generation_) {
  // 开启新的恢复代次，旧代次的订单簿立即失效。
  observed_generation_ = generation_->fetch_add(1) + 1;

  // 绑定增量接收端口并在指定接口加入组播组。
  ASSERT(incremental_.init(inc_ip, iface, inc_port, true) >= 0 && incremental_.join(inc_ip),
         "cannot subscribe incremental stream");

  // 绑定快照接收端口并在指定接口加入组播组。
  ASSERT(snapshot_.init(snap_ip, iface, snap_port, true) >= 0 && snapshot_.join(snap_ip),
         "cannot subscribe snapshot stream");

  // 两路 socket 共用解析回调，内部按对象身份区分快照。
  incremental_.recv_callback_ = [this](auto* socket) { receive(socket); };
  snapshot_.recv_callback_ = incremental_.recv_callback_;
}

// 使当前行情失效并重新开始恢复。
void MarketDataConsumer::stale() {
  // 推进代次使旧订单簿失效；旧批次的结束标记无法恢复当前代次。
  observed_generation_ = generation_->fetch_add(1, std::memory_order_acq_rel) + 1;

  // 清空恢复缓存和候选，记录队列背压情况。
  recovery_.restart();
  candidate_.reset();
  logger_.log("MARKET STALE: generation:% queue_full:%\n", observed_generation_,
              output_->full_count());
}

// 行情接收线程循环。
void MarketDataConsumer::run() {
  while (run_) {  // 运行标志为真时持续轮询。
    // 交易引擎可能主动要求新一轮恢复。
    if (generation_->load(std::memory_order_acquire) != observed_generation_) {
      // 同步外部代次并丢弃旧候选。
      observed_generation_ = generation_->load();
      recovery_.restart();
      candidate_.reset();
    }

    // 轮询两路 UDP，并重试待发布的完整恢复批次。
    incremental_.sendAndRecv();
    snapshot_.sendAndRecv();
    publishRecovery();
  }

  logger_.log(
      "RECOVERY COUNTERS gaps:% duplicates:% cache_overflow:% bad_hash:% recoveries:% queue_full:% high_watermark:%\n",
      recovery_.gaps_, recovery_.duplicates_, recovery_.overflows_, recovery_.hash_failures_,
      recovery_.recoveries_, output_->full_count(), output_->high_watermark());
}

// 验证单个 UDP 数据报并逐帧解码。
void MarketDataConsumer::receive(Common::McastSocket* socket) {
  const bool snapshot = socket == &snapshot_;  // 通道由接收 socket 确定，不由报文自己声明。
  if (snapshot && !recovery_.recovering()) {
    socket->next_rcv_valid_index_ = 0;  // 实时状态丢弃周期快照，损坏快照也不影响增量行情。
    return;
  }

  // 截断或长度不是帧长整数倍，丢弃该报文并进入恢复。
  if (socket->receive_fault_ || socket->next_rcv_valid_index_ % Common::Wire::MarketSize) {
    socket->next_rcv_valid_index_ = 0;
    stale();
    return;
  }

  // 按固定 MarketSize 步长遍历完整帧。
  for (size_t offset = 0; offset < socket->next_rcv_valid_index_;
       offset += Common::Wire::MarketSize) {
    Exchange::MDPMarketUpdate update;  // 保存一条解码结果。

    // 解码失败时使行情失效并结束本报文处理。
    if (!Common::Wire::decode(
            reinterpret_cast<const uint8_t*>(socket->inbound_data_.data() + offset), update)) {
      stale();
      break;
    }
    const auto& event = update.me_market_update_;  // 取得业务事件，检查增量通道的约束。
    if (!snapshot && (event.type_ < Exchange::MarketUpdateType::CLEAR ||
                      event.type_ > Exchange::MarketUpdateType::TRADE ||
                      event.ticker_id_ >= Common::ME_MAX_TICKERS || update.snapshot_cycle_ ||
                      update.watermark_ || update.state_hash_)) {
      stale();  // 快照控制帧和快照元数据不能混入增量队列，非法品种也不能交给订单簿。
      break;
    }
    process(snapshot, update);  // 通过通道校验后交给实时或恢复路径。
  }

  socket->next_rcv_valid_index_ = 0;  // 本报文处理结束，清空接收有效长度。
}

// 处理一条合法编码的行情帧。
void MarketDataConsumer::process(bool snapshot, const Exchange::MDPMarketUpdate& update) {
  if (snapshot && !recovery_.recovering())
    return;                                    // 实时状态不采用新的周期快照。
  if (!snapshot && !recovery_.recovering()) {  // 实时状态下优先检查增量连续性。
    if (recovery_.liveCandidate(update)) {     // 判断该序号是否为下一条连续增量。
      // 进入下游队列后才推进实时期望序号。
      if (output_->try_push(update.me_market_update_)) {
        recovery_.commitLive();
        return;
      }
      stale();  // 队列满使本地行情失效，转为快照恢复。
    } else if (!recovery_.recovering())
      return;  // 重复旧帧直接忽略，不触发恢复。
    else {
      // 检测到缺口时建立新代次，旧订单簿不能继续驱动策略。
      observed_generation_ = generation_->fetch_add(1) + 1;
      candidate_.reset();
    }
  }

  // 缓存输入并尝试构造完整、连续、哈希正确的恢复批次。
  recovery_.ingest(snapshot, update);
  candidate_ = recovery_.build();
  if (candidate_) {  // 批次通过恢复校验。
    // 附加轻量结束标记，携带最终水位和恢复代次。
    Exchange::MEMarketUpdate marker;
    marker.type_ = Exchange::MarketUpdateType::RECOVERY_COMMIT;
    marker.order_id_ = candidate_->watermark;
    marker.priority_ = observed_generation_;

    // 将提交标记置于批次末尾，然后尝试整批发布。
    candidate_->events.push_back(marker);
    publishRecovery();
  }
}

// 重试恢复批次的队列准入。
void MarketDataConsumer::publishRecovery() {
  if (!candidate_)
    return;                                           // 没有候选时无需处理。
  if (output_->try_push_batch(candidate_->events)) {  // 空间够才一次发布全批，失败一个元素也不写。
    logger_.log("RECOVERY PUBLISHED watermark:% generation:% events:%\n", candidate_->watermark,
                observed_generation_, candidate_->events.size());

    // 发布成功后清除候选；订单簿是否已应用到本代次，由消费进度决定。
    recovery_.commit(*candidate_);
    candidate_.reset();

    // 空间不足退让 1ms，此时两路 socket 的轮询也会暂停。
  } else
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
}
