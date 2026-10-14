#pragma once

#include <array>
#include <map>
#include <sstream>
#include "common/types.h"

using namespace Common;

namespace Trading {
/// 客户端行情订单簿中的单笔订单。
struct MarketOrder {
  OrderId order_id_ = OrderId_INVALID;
  Side side_ = Side::INVALID;
  Price price_ = Price_INVALID;
  Qty qty_ = Qty_INVALID;
  Priority priority_ = Priority_INVALID;

  /// 同时充当同一价格档内 FIFO 双向环的节点。
  MarketOrder* prev_order_ = nullptr;
  MarketOrder* next_order_ = nullptr;

  /// 对象池需要的默认构造入口。
  MarketOrder() = default;

  MarketOrder(OrderId order_id, Side side, Price price, Qty qty, Priority priority,
              MarketOrder* prev_order, MarketOrder* next_order) noexcept
      : order_id_(order_id),
        side_(side),
        price_(price),
        qty_(qty),
        priority_(priority),
        prev_order_(prev_order),
        next_order_(next_order) {}

  auto toString() const -> std::string;
};

/// 按市场订单 ID 索引的订单指针数组。
typedef std::array<MarketOrder*, ME_MAX_ORDER_IDS> OrderHashMap;

/// 客户端行情订单簿中的价格档。
/// 档内订单按 FIFO 排列。
struct MarketOrdersAtPrice {
  Side side_ = Side::INVALID;
  Price price_ = Price_INVALID;

  MarketOrder* first_mkt_order_ = nullptr;

  /// 同时充当价格双向环的节点；从最优价走向更差的价格。
  MarketOrdersAtPrice* prev_entry_ = nullptr;
  MarketOrdersAtPrice* next_entry_ = nullptr;

  /// 对象池需要的默认构造入口。
  MarketOrdersAtPrice() = default;

  MarketOrdersAtPrice(Side side, Price price, MarketOrder* first_mkt_order,
                      MarketOrdersAtPrice* prev_entry, MarketOrdersAtPrice* next_entry)
      : side_(side),
        price_(price),
        first_mkt_order_(first_mkt_order),
        prev_entry_(prev_entry),
        next_entry_(next_entry) {}

  auto toString() const {
    std::stringstream ss;
    ss << "MarketOrdersAtPrice["
       << "side:" << sideToString(side_) << " "
       << "price:" << priceToString(price_) << " "
       << "first_mkt_order:" << (first_mkt_order_ ? first_mkt_order_->toString() : "null") << " "
       << "prev:" << priceToString(prev_entry_ ? prev_entry_->price_ : Price_INVALID) << " "
       << "next:" << priceToString(next_entry_ ? next_entry_->price_ : Price_INVALID) << "]";

    return ss.str();
  }
};

/// 按买卖方向和价格查找价格档。
typedef std::map<PriceKey, MarketOrdersAtPrice*> OrdersAtPriceHashMap;

/// 最优买卖报价 BBO，只保留顶档价格和总量，供不需要全簿的组件使用。
using BookQty = uint64_t;
constexpr BookQty BookQty_INVALID = UINT64_MAX;

struct BBO {
  Price bid_price_ = Price_INVALID, ask_price_ = Price_INVALID;
  BookQty bid_qty_ = BookQty_INVALID, ask_qty_ = BookQty_INVALID;

  auto toString() const {
    std::stringstream ss;
    ss << "BBO{" << (bid_qty_ == BookQty_INVALID ? "INVALID" : std::to_string(bid_qty_)) << "@"
       << priceToString(bid_price_) << "X" << priceToString(ask_price_) << "@"
       << (ask_qty_ == BookQty_INVALID ? "INVALID" : std::to_string(ask_qty_)) << "}";

    return ss.str();
  };
};
}
