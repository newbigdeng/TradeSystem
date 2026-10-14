#pragma once

#include <sstream>

#include "common/types.h"
#include "common/lf_queue.h"

using namespace Common;

namespace Exchange {
/// 行情事件类型。
enum class MarketUpdateType : uint8_t {
  INVALID = 0,
  CLEAR = 1,
  ADD = 2,
  MODIFY = 3,
  CANCEL = 4,
  TRADE = 5,
  SNAPSHOT_START = 6,
  SNAPSHOT_END = 7,
  RECOVERY_COMMIT = 8
};

inline std::string marketUpdateTypeToString(MarketUpdateType type) {
  switch (type) {
    case MarketUpdateType::CLEAR:
      return "CLEAR";
    case MarketUpdateType::ADD:
      return "ADD";
    case MarketUpdateType::MODIFY:
      return "MODIFY";
    case MarketUpdateType::CANCEL:
      return "CANCEL";
    case MarketUpdateType::TRADE:
      return "TRADE";
    case MarketUpdateType::SNAPSHOT_START:
      return "SNAPSHOT_START";
    case MarketUpdateType::SNAPSHOT_END:
      return "SNAPSHOT_END";
    case MarketUpdateType::INVALID:
      return "INVALID";
    case MarketUpdateType::RECOVERY_COMMIT:
      return "RECOVERY_COMMIT";
  }
  return "UNKNOWN";
}

/// 内部消息使用自然对齐；网络传输采用带版本号的显式大端编码。

/// 撮合引擎使用的内部行情消息。
struct MEMarketUpdate {
  MarketUpdateType type_ = MarketUpdateType::INVALID;

  OrderId order_id_ = OrderId_INVALID;
  TickerId ticker_id_ = TickerId_INVALID;
  Side side_ = Side::INVALID;
  Price price_ = Price_INVALID;
  Qty qty_ = Qty_INVALID;
  Priority priority_ = Priority_INVALID;

  auto toString() const {
    std::stringstream ss;
    ss << "MEMarketUpdate"
       << " ["
       << " type:" << marketUpdateTypeToString(type_) << " ticker:" << tickerIdToString(ticker_id_)
       << " oid:" << orderIdToString(order_id_) << " side:" << sideToString(side_)
       << " qty:" << qtyToString(qty_) << " price:" << priceToString(price_)
       << " priority:" << priorityToString(priority_) << "]";
    return ss.str();
  }
};

/// 发布到网络的行情帧，包含序号及快照元数据。
struct MDPMarketUpdate {
  uint64_t seq_num_ = 0;
  MEMarketUpdate me_market_update_;
  uint64_t snapshot_cycle_ = 0, watermark_ = 0, state_hash_ = 0;

  auto toString() const {
    std::stringstream ss;
    ss << "MDPMarketUpdate"
       << " ["
       << " seq:" << seq_num_ << " " << me_market_update_.toString() << "]";
    return ss.str();
  }
};

/// 分别传递内部行情和网络行情帧的无锁队列。
typedef Common::LFQueue<Exchange::MEMarketUpdate> MEMarketUpdateLFQueue;
typedef Common::LFQueue<Exchange::MDPMarketUpdate> MDPMarketUpdateLFQueue;
}
