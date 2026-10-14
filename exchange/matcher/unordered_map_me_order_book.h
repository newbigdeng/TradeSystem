#pragma once
#include <functional>
#include <unordered_set>

#include <unordered_map>

#include "common/types.h"
#include "common/mem_pool.h"
#include "common/logging.h"
#include "order_server/client_response.h"
#include "market_data/market_update.h"

#include "me_order.h"

using namespace Common;

namespace Exchange {
class MatchingEngine;

class UnorderedMapMEOrderBook final {
 public:
  explicit UnorderedMapMEOrderBook(TickerId ticker_id, Logger* logger,
                                   MatchingEngine* matching_engine);

  ~UnorderedMapMEOrderBook();

  /// 新增订单。
  /// 先撮合对手方，剩余数量再挂入订单簿。
  auto add(ClientId client_id, OrderId client_order_id, TickerId ticker_id, Side side, Price price,
           Qty qty) noexcept -> void;

  /// 撤销已有订单；找不到订单时返回撤单拒绝。
  auto cancel(ClientId client_id, OrderId order_id, TickerId ticker_id) noexcept -> void;

  std::function<void(const MEClientResponse&)> response_sink_;
  std::function<void(const MEMarketUpdate&)> market_sink_;

  std::vector<MEMarketUpdate> liveOrders() const {
    std::vector<MEMarketUpdate> result;
    for (const auto& client : cid_oid_to_order_)
      for (const auto& [id, order] : client) {
        (void)id;
        result.push_back({MarketUpdateType::ADD, order->market_order_id_, ticker_id_, order->side_,
                          order->price_, order->qty_, order->priority_});
      }
    std::sort(result.begin(), result.end(),
              [](const auto& a, const auto& b) { return a.order_id_ < b.order_id_; });
    return result;
  }

  void query(ClientId client_id, OrderId order_id);
  auto toString(bool detailed, bool validity_check) const -> std::string;

  /// 禁止默认构造、复制和移动，避免重复拥有资源。
  UnorderedMapMEOrderBook() = delete;

  UnorderedMapMEOrderBook(const UnorderedMapMEOrderBook&) = delete;

  UnorderedMapMEOrderBook(const UnorderedMapMEOrderBook&&) = delete;

  UnorderedMapMEOrderBook& operator=(const UnorderedMapMEOrderBook&) = delete;

  UnorderedMapMEOrderBook& operator=(const UnorderedMapMEOrderBook&&) = delete;

 private:
  TickerId ticker_id_ = TickerId_INVALID;

  /// 所属撮合引擎，用于发布回报和行情。
  MatchingEngine* matching_engine_ = nullptr;

  /// 先按客户端 ID 索引，再按其订单 ID 查找。
  ClientOrderHashMap cid_oid_to_order_{};

  /// 价格档对象池。
  MemPool<MEOrdersAtPrice> orders_at_price_pool_;

  /// 买卖两侧的最优价格档，也是各自价格环的起点。
  MEOrdersAtPrice* bids_by_price_ = nullptr;
  MEOrdersAtPrice* asks_by_price_ = nullptr;

  /// 按买卖方向和价格查找价格档。
  std::unordered_map<PriceKey, MEOrdersAtPrice*, PriceKeyHash> price_orders_at_price_;

  /// 订单对象池。
  MemPool<MEOrder> order_pool_;

  /// 复用的回报和行情消息对象。
  MEClientResponse client_response_;
  MEMarketUpdate market_update_;

  std::array<std::unordered_map<OrderId, MEClientResponse>, ME_MAX_NUM_CLIENTS>
      last_order_response_{};
  OrderId next_market_order_id_ = 1;
  std::array<std::unordered_set<OrderId>, ME_MAX_NUM_CLIENTS> used_client_order_ids_{};
  void emitResponse(const MEClientResponse& response);
  void emitMarket(const MEMarketUpdate& update);

  std::string time_str_;
  Logger* logger_ = nullptr;

 private:
  auto generateNewMarketOrderId() noexcept -> OrderId {
    return next_market_order_id_++;
  }

  auto getOrdersAtPrice(Price price, Side side) const noexcept -> MEOrdersAtPrice* {
    const auto found = price_orders_at_price_.find({side, price});
    return found == price_orders_at_price_.end() ? nullptr : found->second;
  }

  /// 登记价格档：买侧从高到低，卖侧从低到高，统一插在 target 前。
  auto addOrdersAtPrice(MEOrdersAtPrice* level) noexcept {
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

  auto getNextPriority(Price price, Side side) noexcept {
    const auto orders_at_price = getOrdersAtPrice(price, side);
    if (!orders_at_price)
      return 1lu;

    return orders_at_price->first_me_order_->prev_order_->priority_ + 1;
  }

  /// 按挂单价格撮合，向双方发送成交回报并发布行情。
  /// 更新挂单剩余量；完全成交后移除挂单。
  /// 通过 leaves_qty 返回主动单的剩余量。
  auto match(TickerId ticker_id, ClientId client_id, Side side, OrderId client_order_id,
             OrderId new_market_order_id, MEOrder* bid_itr, Qty* leaves_qty) noexcept;

  /// 检查新订单能否与对手方挂单成交。
  /// 逐笔调用 match()，返回新订单尚未成交的数量。
  auto checkForMatch(ClientId client_id, OrderId client_order_id, TickerId ticker_id, Side side,
                     Price price, Qty qty, OrderId new_market_order_id) noexcept;

  /// 移除订单并归还对象池。
  auto removeOrder(MEOrder* order) noexcept {
    auto orders_at_price = getOrdersAtPrice(order->price_, order->side_);

    if (order->prev_order_ == order) {  // 档内只剩这一笔订单。
      removeOrdersAtPrice(order->side_, order->price_);
    } else {  // 将该订单从双向环摘除。
      const auto order_before = order->prev_order_;
      const auto order_after = order->next_order_;
      order_before->next_order_ = order_after;
      order_after->prev_order_ = order_before;

      if (orders_at_price->first_me_order_ == order) {
        orders_at_price->first_me_order_ = order_after;
      }

      order->prev_order_ = order->next_order_ = nullptr;
    }

    cid_oid_to_order_.at(order->client_id_).erase(order->client_order_id_);
    order_pool_.deallocate(order);
  }

  /// 把新订单追加到所属价格档的 FIFO 尾部。
  auto addOrder(MEOrder* order) noexcept {
    const auto orders_at_price = getOrdersAtPrice(order->price_, order->side_);

    if (!orders_at_price) {
      order->next_order_ = order->prev_order_ = order;

      auto new_orders_at_price =
          orders_at_price_pool_.allocate(order->side_, order->price_, order, nullptr, nullptr);
      addOrdersAtPrice(new_orders_at_price);
    } else {
      auto first_order = orders_at_price->first_me_order_;

      first_order->prev_order_->next_order_ = order;
      order->prev_order_ = first_order->prev_order_;
      order->next_order_ = first_order;
      first_order->prev_order_ = order;
    }

    cid_oid_to_order_[order->client_id_][order->client_order_id_] = order;
  }
};
}
