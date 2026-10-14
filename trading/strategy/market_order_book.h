#pragma once
#include <set>

#include "common/types.h"
#include "common/mem_pool.h"
#include "common/logging.h"

#include "market_order.h"
#include "exchange/market_data/market_update.h"

namespace Trading {
class TradeEngine;

class MarketOrderBook final {
 public:
  MarketOrderBook(TickerId ticker_id, Logger* logger);

  ~MarketOrderBook();

  /// 验证并应用行情事件，更新本地订单簿。
  auto onMarketUpdate(const Exchange::MEMarketUpdate* market_update) noexcept -> bool;

  auto setTradeEngine(TradeEngine* trade_engine) {
    trade_engine_ = trade_engine;
  }

  /// 更新最优买卖报价；两个参数分别指定是否更新买侧、卖侧。
  auto updateBBO(bool update_bid, bool update_ask) noexcept {
    if (update_bid) {
      if (bids_by_price_) {
        bbo_.bid_price_ = bids_by_price_->price_;
        bbo_.bid_qty_ = bids_by_price_->first_mkt_order_->qty_;
        for (auto order = bids_by_price_->first_mkt_order_->next_order_;
             order != bids_by_price_->first_mkt_order_; order = order->next_order_)
          bbo_.bid_qty_ += order->qty_;
      } else {
        bbo_.bid_price_ = Price_INVALID;
        bbo_.bid_qty_ = BookQty_INVALID;
      }
    }

    if (update_ask) {
      if (asks_by_price_) {
        bbo_.ask_price_ = asks_by_price_->price_;
        bbo_.ask_qty_ = asks_by_price_->first_mkt_order_->qty_;
        for (auto order = asks_by_price_->first_mkt_order_->next_order_;
             order != asks_by_price_->first_mkt_order_; order = order->next_order_)
          bbo_.ask_qty_ += order->qty_;
      } else {
        bbo_.ask_price_ = Price_INVALID;
        bbo_.ask_qty_ = BookQty_INVALID;
      }
    }
  }

  std::vector<Exchange::MEMarketUpdate> liveOrders() const {
    std::vector<Exchange::MEMarketUpdate> result;
    for (const auto id : live_order_ids_) {
      auto* order = oid_to_order_[id];
      result.push_back({Exchange::MarketUpdateType::ADD, order->order_id_, ticker_id_, order->side_,
                        order->price_, order->qty_, order->priority_});
    }
    return result;
  }

  auto getBBO() const noexcept -> const BBO* {
    return &bbo_;
  }

  auto toString(bool detailed, bool validity_check) const -> std::string;

  /// 禁止默认构造、复制和移动，避免重复拥有资源。
  MarketOrderBook() = delete;

  MarketOrderBook(const MarketOrderBook&) = delete;

  MarketOrderBook(const MarketOrderBook&&) = delete;

  MarketOrderBook& operator=(const MarketOrderBook&) = delete;

  MarketOrderBook& operator=(const MarketOrderBook&&) = delete;

 private:
  const TickerId ticker_id_;

  /// 所属交易引擎，用于通知订单簿变化和成交事件。
  TradeEngine* trade_engine_ = nullptr;

  /// 按市场订单 ID 索引的订单指针数组。
  OrderHashMap oid_to_order_{};
  std::set<OrderId> live_order_ids_;

  /// 行情价格档对象池。
  MemPool<MarketOrdersAtPrice> orders_at_price_pool_;

  /// 买卖两侧的最优价格档，也是各自价格环的起点。
  MarketOrdersAtPrice* bids_by_price_ = nullptr;
  MarketOrdersAtPrice* asks_by_price_ = nullptr;

  /// 按买卖方向和价格查找价格档。
  OrdersAtPriceHashMap price_orders_at_price_;

  /// 行情订单对象池。
  MemPool<MarketOrder> order_pool_;

  BBO bbo_;

  std::string time_str_;
  Logger* logger_ = nullptr;

 private:
  auto getOrdersAtPrice(Price price, Side side) const noexcept -> MarketOrdersAtPrice* {
    const auto found = price_orders_at_price_.find({side, price});
    return found == price_orders_at_price_.end() ? nullptr : found->second;
  }

  /// 登记价格档：买侧从高到低，卖侧从低到高，统一插在 target 前。
  auto addOrdersAtPrice(MarketOrdersAtPrice* level) noexcept {
    price_orders_at_price_[{level->side_, level->price_}] = level;
    const bool buy = level->side_ == Side::BUY;
    auto*& best = buy ? bids_by_price_ : asks_by_price_;
    if (UNLIKELY(!best)) {
      best = level;
      level->prev_entry_ = level->next_entry_ = level;
      return;
    }

    const auto better = [buy](Price left, Price right) {
      return buy ? left > right : left < right;
    };
    const bool new_best = better(level->price_, best->price_);
    auto* target = best;

    // best 的前驱就是最差档；新最优、新最差都可直接插入，无需遍历价格环。
    if (!new_best && better(level->price_, best->prev_entry_->price_)) {
      do {
        target = target->next_entry_;
      } while (target != best && better(target->price_, level->price_));
    }

    level->prev_entry_ = target->prev_entry_;
    level->next_entry_ = target;
    target->prev_entry_->next_entry_ = level;
    target->prev_entry_ = level;
    if (new_best)
      best = level;
  }

  /// 移除价格档并归还对象池。
  auto removeOrdersAtPrice(Side side, Price price) noexcept {
    const auto best_orders_by_price = (side == Side::BUY ? bids_by_price_ : asks_by_price_);
    auto orders_at_price = getOrdersAtPrice(price, side);

    if (UNLIKELY(orders_at_price->next_entry_ == orders_at_price)) {  // 该侧只剩最后一个价格档。
      (side == Side::BUY ? bids_by_price_ : asks_by_price_) = nullptr;
    } else {
      orders_at_price->prev_entry_->next_entry_ = orders_at_price->next_entry_;
      orders_at_price->next_entry_->prev_entry_ = orders_at_price->prev_entry_;

      if (orders_at_price == best_orders_by_price) {
        (side == Side::BUY ? bids_by_price_ : asks_by_price_) = orders_at_price->next_entry_;
      }

      orders_at_price->prev_entry_ = orders_at_price->next_entry_ = nullptr;
    }

    price_orders_at_price_.erase({side, price});

    orders_at_price_pool_.deallocate(orders_at_price);
  }

  /// 移除订单并归还对象池。
  auto removeOrder(MarketOrder* order) noexcept -> void {
    auto orders_at_price = getOrdersAtPrice(order->price_, order->side_);

    if (order->prev_order_ == order) {  // 档内只剩这一笔订单。
      removeOrdersAtPrice(order->side_, order->price_);
    } else {  // 将该订单从双向环摘除。
      const auto order_before = order->prev_order_;
      const auto order_after = order->next_order_;
      order_before->next_order_ = order_after;
      order_after->prev_order_ = order_before;

      if (orders_at_price->first_mkt_order_ == order) {
        orders_at_price->first_mkt_order_ = order_after;
      }

      order->prev_order_ = order->next_order_ = nullptr;
    }

    oid_to_order_.at(order->order_id_) = nullptr;
    live_order_ids_.erase(order->order_id_);
    order_pool_.deallocate(order);
  }

  /// 把新订单追加到所属价格档的 FIFO 尾部。
  auto addOrder(MarketOrder* order) noexcept -> void {
    const auto orders_at_price = getOrdersAtPrice(order->price_, order->side_);

    if (!orders_at_price) {
      order->next_order_ = order->prev_order_ = order;

      auto new_orders_at_price =
          orders_at_price_pool_.allocate(order->side_, order->price_, order, nullptr, nullptr);
      addOrdersAtPrice(new_orders_at_price);
    } else {
      auto first_order = orders_at_price->first_mkt_order_;

      first_order->prev_order_->next_order_ = order;
      order->prev_order_ = first_order->prev_order_;
      order->next_order_ = first_order;
      first_order->prev_order_ = order;
    }

    oid_to_order_.at(order->order_id_) = order;
    live_order_ids_.insert(order->order_id_);
  }
};

/// 按品种 ID 索引的行情订单簿数组。
typedef std::array<MarketOrderBook*, ME_MAX_TICKERS> MarketOrderBookHashMap;
}
