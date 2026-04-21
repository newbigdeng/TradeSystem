#pragma once

#include <array>
#include <sstream>
#include "common/types.h"

using namespace Common;

namespace Exchange {
  /// 匹配引擎用来表示限价订单簿中的单个订单。
  struct MEOrder {
    TickerId ticker_id_ = TickerId_INVALID;
    ClientId client_id_ = ClientId_INVALID;
    OrderId client_order_id_ = OrderId_INVALID;
    OrderId market_order_id_ = OrderId_INVALID;
    Side side_ = Side::INVALID;
    Price price_ = Price_INVALID;
    Qty qty_ = Qty_INVALID;
    Priority priority_ = Priority_INVALID;

    /// MEOrder还用作按FIFO顺序排列的价格水平上所有订单的双向链表中的节点。
    MEOrder *prev_order_ = nullptr;
    MEOrder *next_order_ = nullptr;

    /// 仅在与MemPool一起使用时需要。
    MEOrder() = default;

    MEOrder(TickerId ticker_id, ClientId client_id, OrderId client_order_id, OrderId market_order_id, Side side, Price price,
                     Qty qty, Priority priority, MEOrder *prev_order, MEOrder *next_order) noexcept
        : ticker_id_(ticker_id), client_id_(client_id), client_order_id_(client_order_id), market_order_id_(market_order_id), side_(side),
          price_(price), qty_(qty), priority_(priority), prev_order_(prev_order), next_order_(next_order) {}

    auto toString() const -> std::string;
  };

  /// Hash map from OrderId -> MEOrder.
  typedef std::array<MEOrder *, ME_MAX_ORDER_IDS> OrderHashMap;

  /// Hash map from ClientId -> OrderId -> MEOrder.
  typedef std::array<OrderHashMap, ME_MAX_NUM_CLIENTS> ClientOrderHashMap;

  /// 匹配引擎用来表示限价订单簿中的价格水平。
  /// 内部维护按FIFO顺序排列的MEOrder对象列表。
  struct MEOrdersAtPrice {
    Side side_ = Side::INVALID;
    Price price_ = Price_INVALID;

    MEOrder *first_me_order_ = nullptr;

    /// MEOrdersAtPrice还用作按最积极到最不积极价格顺序排列的价格水平双向链表中的节点。
    MEOrdersAtPrice *prev_entry_ = nullptr;
    MEOrdersAtPrice *next_entry_ = nullptr;

    /// 仅在与MemPool一起使用时需要。
    MEOrdersAtPrice() = default;

    MEOrdersAtPrice(Side side, Price price, MEOrder *first_me_order, MEOrdersAtPrice *prev_entry, MEOrdersAtPrice *next_entry)
        : side_(side), price_(price), first_me_order_(first_me_order), prev_entry_(prev_entry), next_entry_(next_entry) {}

    auto toString() const {
      std::stringstream ss;
      ss << "MEOrdersAtPrice["
         << "side:" << sideToString(side_) << " "
         << "price:" << priceToString(price_) << " "
         << "first_me_order:" << (first_me_order_ ? first_me_order_->toString() : "null") << " "
         << "prev:" << priceToString(prev_entry_ ? prev_entry_->price_ : Price_INVALID) << " "
         << "next:" << priceToString(next_entry_ ? next_entry_->price_ : Price_INVALID) << "]";

      return ss.str();
    }
  };

  /// Hash map from Price -> MEOrdersAtPrice.
  typedef std::array<MEOrdersAtPrice *, ME_MAX_PRICE_LEVELS> OrdersAtPriceHashMap;
}