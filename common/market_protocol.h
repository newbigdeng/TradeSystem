#pragma once
#include "order_protocol.h"
#include "exchange/market_data/market_update.h"

namespace Common::Wire {
constexpr size_t MarketSize = 70;  // 固定帧长，不依赖内部结构体的 sizeof 或填充字节。

inline Bytes<MarketSize> encode(const Exchange::MDPMarketUpdate& message) noexcept {
  Bytes<MarketSize> bytes{};  // 先清零，所有字段随后按大端顺序写入。
  auto* p = bytes.data();     // put 每写一个字段就推进游标。
  put(p, uint8_t('T'));       // 协议魔数的第一个字节。
  put(p, uint8_t('S'));       // 协议魔数的第二个字节。
  put(p, Version);            // 协议版本。
  put(p, uint8_t(3));         // 消息种类 3 表示行情。

  put(p, message.seq_num_);         // 增量序号或快照周期内序号。
  put(p, message.snapshot_cycle_);  // 快照周期，避免混用不同周期的帧。
  put(p, message.watermark_);       // 快照已经覆盖的最后一条增量序号。
  put(p, message.state_hash_);      // 发布端声明的快照状态校验值。

  const auto& r = message.me_market_update_;  // 取内部行情，只编码显式业务字段。
  put(p, static_cast<uint8_t>(r.type_));      // 公开行情事件类型。
  put(p, r.order_id_);                        // 市场订单 ID；首尾标记用它携带水位。
  put(p, r.ticker_id_);                       // 品种 ID。
  put(p, static_cast<int8_t>(r.side_));       // 买卖方向按有符号单字节编码。
  put(p, r.price_);                           // 固定宽度整数价格。
  put(p, r.qty_);                             // 数量；MODIFY 表示绝对剩余量。
  put(p, r.priority_);                        // 同价订单的 FIFO 优先级。
  return bytes;
}

// 调用者须先确认收到完整的 MarketSize 字节，再进入逐字段解码。
inline bool decode(const uint8_t* p, Exchange::MDPMarketUpdate& message) noexcept {
  if (get<uint8_t>(p) != 'T' || get<uint8_t>(p) != 'S' || get<uint8_t>(p) != Version ||
      get<uint8_t>(p) != 3)
    return false;  // 拒绝不匹配的魔数、版本或消息种类。

  message.seq_num_ = get<uint64_t>(p);         // get 按大端读取并推进游标。
  message.snapshot_cycle_ = get<uint64_t>(p);  // 读取快照周期。
  message.watermark_ = get<uint64_t>(p);       // 读取快照水位。
  message.state_hash_ = get<uint64_t>(p);      // 读取快照声明校验值。

  auto& r = message.me_market_update_;                                 // 将业务字段写入内部事件。
  r.type_ = static_cast<Exchange::MarketUpdateType>(get<uint8_t>(p));  // 读取类型。
  r.order_id_ = get<uint64_t>(p);               // 读取订单 ID 或首尾标记水位。
  r.ticker_id_ = get<uint32_t>(p);              // 读取品种 ID。
  r.side_ = static_cast<Side>(get<int8_t>(p));  // 读取有符号方向值。
  r.price_ = get<int64_t>(p);                   // 读取整数价格。
  r.qty_ = get<uint32_t>(p);                    // 读取数量。
  r.priority_ = get<uint64_t>(p);               // 读取优先级。

  // RECOVERY_COMMIT 仅供进程内队列使用，网络不得直接开放策略。
  return r.type_ >= Exchange::MarketUpdateType::CLEAR &&
         r.type_ <= Exchange::MarketUpdateType::SNAPSHOT_END;
}
}
