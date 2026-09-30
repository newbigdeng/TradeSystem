#pragma once

#include <sstream>

#include "common/types.h"
#include "common/lf_queue.h"

using namespace Common;

namespace Exchange {
  /// Represents the type / action in the market update message.
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

  /// Internal aligned messages. The transport uses explicit versioned big-endian encoders.

  /// Market update structure used internally by the matching engine.
  struct MEMarketUpdate {
    MarketUpdateType type_ = MarketUpdateType::INVALID;

    OrderId order_id_ = OrderId_INVALID;
    TickerId ticker_id_ = TickerId_INVALID;
    Side side_ = Side::INVALID;
    Price price_ = Price_INVALID;
    Qty qty_ = Qty_INVALID;
    Priority priority_ = Priority_INVALID;
    uint64_t state_hash_=0;

    auto toString() const {
      std::stringstream ss;
      ss << "MEMarketUpdate"
         << " ["
         << " type:" << marketUpdateTypeToString(type_)
         << " ticker:" << tickerIdToString(ticker_id_)
         << " oid:" << orderIdToString(order_id_)
         << " side:" << sideToString(side_)
         << " qty:" << qtyToString(qty_)
         << " price:" << priceToString(price_)
         << " priority:" << priorityToString(priority_)
         << "]";
      return ss.str();
    }
  };

  /// Market update structure published over the network by the market data publisher.
  struct MDPMarketUpdate {
    uint64_t seq_num_ = 0;
    MEMarketUpdate me_market_update_;
    uint64_t snapshot_cycle_=0,watermark_=0,state_hash_=0;

    auto toString() const {
      std::stringstream ss;
      ss << "MDPMarketUpdate"
         << " ["
         << " seq:" << seq_num_
         << " " << me_market_update_.toString()
         << "]";
      return ss.str();
    }
  };


  /// Lock free queues of matching engine market update messages and market data publisher market updates messages respectively.
  typedef Common::LFQueue<Exchange::MEMarketUpdate> MEMarketUpdateLFQueue;
  typedef Common::LFQueue<Exchange::MDPMarketUpdate> MDPMarketUpdateLFQueue;
}
