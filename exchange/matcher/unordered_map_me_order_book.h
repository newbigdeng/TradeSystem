#pragma once

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
    explicit UnorderedMapMEOrderBook(TickerId ticker_id, Logger *logger, MatchingEngine *matching_engine);

    ~UnorderedMapMEOrderBook();

    /// 使用提供的属性在订单簿中创建并添加新订单。
    /// 它将检查此新订单是否与具有相反方向的现有被动订单匹配，如果是，则执行匹配。
    auto add(ClientId client_id, OrderId client_order_id, TickerId ticker_id, Side side, Price price, Qty qty) noexcept -> void;

    /// 尝试取消订单簿中的订单，如果订单不存在则发出取消拒绝。
    auto cancel(ClientId client_id, OrderId order_id, TickerId ticker_id) noexcept -> void;

    auto toString(bool detailed, bool validity_check) const -> std::string;

    /// 删除默认、拷贝和移动构造函数及赋值运算符。
    UnorderedMapMEOrderBook() = delete;

    UnorderedMapMEOrderBook(const UnorderedMapMEOrderBook &) = delete;

    UnorderedMapMEOrderBook(const UnorderedMapMEOrderBook &&) = delete;

    UnorderedMapMEOrderBook &operator=(const UnorderedMapMEOrderBook &) = delete;

    UnorderedMapMEOrderBook &operator=(const UnorderedMapMEOrderBook &&) = delete;

  private:
    TickerId ticker_id_ = TickerId_INVALID;

    /// 父匹配引擎实例，用于发布市场数据和客户端响应。
    MatchingEngine *matching_engine_ = nullptr;

    /// 从客户端ID到订单ID再到MEOrder的哈希映射。
    std::unordered_map<ClientId, std::unordered_map<OrderId, MEOrder *>> cid_oid_to_order_;

    /// 内存池用于管理MEOrdersAtPrice对象。
    MemPool<MEOrdersAtPrice> orders_at_price_pool_;

    /// 指向买卖价格水平的起始/最佳价格/订单簿顶部的指针。
    MEOrdersAtPrice *bids_by_price_ = nullptr;
    MEOrdersAtPrice *asks_by_price_ = nullptr;

    /// 从价格到MEOrdersAtPrice的哈希映射。
    std::unordered_map<Price, MEOrdersAtPrice *> price_orders_at_price_;

    /// 内存池用于管理MEOrder对象。
    MemPool<MEOrder> order_pool_;

    /// These are used to publish client responses and market updates.
    MEClientResponse client_response_;
    MEMarketUpdate market_update_;

    OrderId next_market_order_id_ = 1;

    std::string time_str_;
    Logger *logger_ = nullptr;

  private:
    auto generateNewMarketOrderId() noexcept -> OrderId {
      return next_market_order_id_++;
    }

    auto priceToIndex(Price price) const noexcept {
      return (price % ME_MAX_PRICE_LEVELS);
    }

    /// 获取并返回与提供价格对应的MEOrdersAtPrice。
    auto getOrdersAtPrice(Price price) const noexcept -> MEOrdersAtPrice * {
      if(price_orders_at_price_.find(priceToIndex(price)) == price_orders_at_price_.end())
        return nullptr;

      return price_orders_at_price_.at(priceToIndex(price));
    }

    /// 在正确价格处将新的MEOrdersAtPrice添加到容器中 - 哈希映射和价格水平的双向链表。
    auto addOrdersAtPrice(MEOrdersAtPrice *new_orders_at_price) noexcept {
      price_orders_at_price_[priceToIndex(new_orders_at_price->price_)] = new_orders_at_price;

      const auto best_orders_by_price = (new_orders_at_price->side_ == Side::BUY ? bids_by_price_ : asks_by_price_);
      if (UNLIKELY(!best_orders_by_price)) {
        (new_orders_at_price->side_ == Side::BUY ? bids_by_price_ : asks_by_price_) = new_orders_at_price;
        new_orders_at_price->prev_entry_ = new_orders_at_price->next_entry_ = new_orders_at_price;
      } else {
        auto target = best_orders_by_price;
        bool add_after = ((new_orders_at_price->side_ == Side::SELL && new_orders_at_price->price_ > target->price_) ||
                          (new_orders_at_price->side_ == Side::BUY && new_orders_at_price->price_ < target->price_));
        if (add_after) {
          target = target->next_entry_;
          add_after = ((new_orders_at_price->side_ == Side::SELL && new_orders_at_price->price_ > target->price_) ||
                       (new_orders_at_price->side_ == Side::BUY && new_orders_at_price->price_ < target->price_));
        }
        while (add_after && target != best_orders_by_price) {
          add_after = ((new_orders_at_price->side_ == Side::SELL && new_orders_at_price->price_ > target->price_) ||
                       (new_orders_at_price->side_ == Side::BUY && new_orders_at_price->price_ < target->price_));
          if (add_after)
            target = target->next_entry_;
        }

        if (add_after) { // add new_orders_at_price after target.
          if (target == best_orders_by_price) {
            target = best_orders_by_price->prev_entry_;
          }
          new_orders_at_price->prev_entry_ = target;
          target->next_entry_->prev_entry_ = new_orders_at_price;
          new_orders_at_price->next_entry_ = target->next_entry_;
          target->next_entry_ = new_orders_at_price;
        } else { // add new_orders_at_price before target.
          new_orders_at_price->prev_entry_ = target->prev_entry_;
          new_orders_at_price->next_entry_ = target;
          target->prev_entry_->next_entry_ = new_orders_at_price;
          target->prev_entry_ = new_orders_at_price;

          if ((new_orders_at_price->side_ == Side::BUY && new_orders_at_price->price_ > best_orders_by_price->price_) ||
              (new_orders_at_price->side_ == Side::SELL && new_orders_at_price->price_ < best_orders_by_price->price_)) {
            target->next_entry_ = (target->next_entry_ == best_orders_by_price ? new_orders_at_price : target->next_entry_);
            (new_orders_at_price->side_ == Side::BUY ? bids_by_price_ : asks_by_price_) = new_orders_at_price;
          }
        }
      }
    }

    /// 从容器中移除MEOrdersAtPrice - 哈希映射和价格水平的双向链表。
    auto removeOrdersAtPrice(Side side, Price price) noexcept {
      const auto best_orders_by_price = (side == Side::BUY ? bids_by_price_ : asks_by_price_);
      auto orders_at_price = getOrdersAtPrice(price);

      if (UNLIKELY(orders_at_price->next_entry_ == orders_at_price)) { // empty side of book.
        (side == Side::BUY ? bids_by_price_ : asks_by_price_) = nullptr;
      } else {
        orders_at_price->prev_entry_->next_entry_ = orders_at_price->next_entry_;
        orders_at_price->next_entry_->prev_entry_ = orders_at_price->prev_entry_;

        if (orders_at_price == best_orders_by_price) {
          (side == Side::BUY ? bids_by_price_ : asks_by_price_) = orders_at_price->next_entry_;
        }

        orders_at_price->prev_entry_ = orders_at_price->next_entry_ = nullptr;
      }

      price_orders_at_price_[priceToIndex(price)] = nullptr;

      orders_at_price_pool_.deallocate(orders_at_price);
    }

    auto getNextPriority(Price price) noexcept {
      const auto orders_at_price = getOrdersAtPrice(price);
      if (!orders_at_price)
        return 1lu;

      return orders_at_price->first_me_order_->prev_order_->priority_ + 1;
    }

    /// 将具有提供参数的新主动订单与bid_itr对象中持有的被动订单进行匹配，并为匹配生成客户端响应和市场更新。
    /// 它将根据匹配更新被动订单(bid_itr)，如果完全匹配则可能移除它。
    /// 它将在leaves_qty参数中返回主动订单的剩余数量。
    auto match(TickerId ticker_id, ClientId client_id, Side side, OrderId client_order_id, OrderId new_market_order_id, MEOrder* bid_itr, Qty* leaves_qty) noexcept;

    /// 检查具有提供属性的新订单是否会与订单簿另一侧的现有被动订单匹配。
    /// 如果有匹配要执行，这将调用match()方法来执行匹配，并返回此新订单上剩余的数量（如果有）。
    auto checkForMatch(ClientId client_id, OrderId client_order_id, TickerId ticker_id, Side side, Price price, Qty qty, Qty new_market_order_id) noexcept;

    /// 从容器中移除并释放提供的订单。
    auto removeOrder(MEOrder *order) noexcept {
      auto orders_at_price = getOrdersAtPrice(order->price_);

      if (order->prev_order_ == order) { // only one element.
        removeOrdersAtPrice(order->side_, order->price_);
      } else { // remove the link.
        const auto order_before = order->prev_order_;
        const auto order_after = order->next_order_;
        order_before->next_order_ = order_after;
        order_after->prev_order_ = order_before;

        if (orders_at_price->first_me_order_ == order) {
          orders_at_price->first_me_order_ = order_after;
        }

        order->prev_order_ = order->next_order_ = nullptr;
      }

      cid_oid_to_order_[order->client_id_][order->client_order_id_] = nullptr;
      order_pool_.deallocate(order);
    }

    /// 在此订单所属的价格水平的FIFO队列末尾添加单个订单。
    auto addOrder(MEOrder *order) noexcept {
      const auto orders_at_price = getOrdersAtPrice(order->price_);

      if (!orders_at_price) {
        order->next_order_ = order->prev_order_ = order;

        auto new_orders_at_price = orders_at_price_pool_.allocate(order->side_, order->price_, order, nullptr, nullptr);
        addOrdersAtPrice(new_orders_at_price);
      } else {
        auto first_order = (orders_at_price ? orders_at_price->first_me_order_ : nullptr);

        first_order->prev_order_->next_order_ = order;
        order->prev_order_ = first_order->prev_order_;
        order->next_order_ = first_order;
        first_order->prev_order_ = order;
      }

      cid_oid_to_order_[order->client_id_][order->client_order_id_] = order;
    }
  };
}