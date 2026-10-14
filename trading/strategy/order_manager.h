#pragma once

#include "common/macros.h"
#include "common/logging.h"

#include "exchange/order_server/client_response.h"

#include "om_order.h"
#include "risk_manager.h"

using namespace Common;

namespace Trading {
class TradeEngine;

/// 管理策略订单，集中处理报单、撤单和回报状态。
class OrderManager {
 public:
  OrderManager(Common::Logger* logger, TradeEngine* trade_engine, RiskManager& risk_manager)
      : trade_engine_(trade_engine), risk_manager_(risk_manager), logger_(logger) {}

  /// 应用客户回报，更新管理中的订单状态。
  auto onOrderUpdate(const Exchange::MEClientResponse* client_response) noexcept -> void {
    logger_->log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(),
                 client_response->toString().c_str());
    if (client_response->ticker_id_ >= ME_MAX_TICKERS)
      return;
    OMOrder* order = nullptr;
    for (auto& candidate : ticker_side_order_[client_response->ticker_id_])
      if (candidate.order_id_ == client_response->client_order_id_ &&
          candidate.order_id_ != OrderId_INVALID) {
        order = &candidate;
        break;
      }
    if (!order)
      return;
    switch (client_response->type_) {
      case Exchange::ClientResponseType::ACCEPTED:
        if (order->order_state_ == OMOrderState::PENDING_NEW)
          order->order_state_ = OMOrderState::LIVE;
        break;
      case Exchange::ClientResponseType::FILLED:
        if (client_response->leaves_qty_ < order->qty_)
          order->qty_ = client_response->leaves_qty_;
        if (!order->qty_)
          order->order_state_ = OMOrderState::DEAD;
        break;
      case Exchange::ClientResponseType::CANCELED:
      case Exchange::ClientResponseType::REJECTED:
        order->qty_ = 0;
        order->order_state_ = OMOrderState::DEAD;
        break;
      case Exchange::ClientResponseType::CANCEL_REJECTED:
        if (order->order_state_ == OMOrderState::PENDING_CANCEL)
          order->order_state_ =
              client_response->leaves_qty_ ? OMOrderState::LIVE : OMOrderState::DEAD;
        break;
      case Exchange::ClientResponseType::STATE:
        order->qty_ = client_response->leaves_qty_;
        order->order_state_ = order->qty_ ? OMOrderState::LIVE : OMOrderState::DEAD;
        break;
      case Exchange::ClientResponseType::INVALID:
        break;
    }
  }

  /// 提交新订单，并更新对应策略订单状态。
  auto newOrder(OMOrder* order, TickerId ticker_id, Price price, Side side, Qty qty) noexcept
      -> void;

  /// 提交撤单，并更新对应策略订单状态。
  auto cancelOrder(OMOrder* order) noexcept -> void;

  /// 将指定方向的订单调整到目标价格和数量。
  /// 发送前检查风险，发送成功后更新策略订单状态。
  auto moveOrder(OMOrder* order, TickerId ticker_id, Price price, Side side, Qty qty) noexcept {
    switch (order->order_state_) {
      case OMOrderState::LIVE: {
        if (order->price_ != price) {
          START_MEASURE(Trading_OrderManager_cancelOrder);
          cancelOrder(order);
          END_MEASURE(Trading_OrderManager_cancelOrder, (*logger_));
        }
      } break;
      case OMOrderState::INVALID:
      case OMOrderState::DEAD: {
        if (LIKELY(price != Price_INVALID)) {
          newOrder(order, ticker_id, price, side, qty);
        }
      } break;
      case OMOrderState::PENDING_NEW:
      case OMOrderState::PENDING_CANCEL:
        break;
    }
  }

  /// 在指定买卖价格维持 clip 数量的挂单。
  /// 没有挂单时发新单。
  /// 价格或数量不匹配时，先撤销已有挂单。
  /// 某侧价格为 Price_INVALID 时，不在该侧挂单。
  auto moveOrders(TickerId ticker_id, Price bid_price, Price ask_price, Qty clip) noexcept {
    {
      auto bid_order = &(ticker_side_order_.at(ticker_id).at(sideToIndex(Side::BUY)));
      START_MEASURE(Trading_OrderManager_moveOrder);
      moveOrder(bid_order, ticker_id, bid_price, Side::BUY, clip);
      END_MEASURE(Trading_OrderManager_moveOrder, (*logger_));
    }

    {
      auto ask_order = &(ticker_side_order_.at(ticker_id).at(sideToIndex(Side::SELL)));
      START_MEASURE(Trading_OrderManager_moveOrder);
      moveOrder(ask_order, ticker_id, ask_price, Side::SELL, clip);
      END_MEASURE(Trading_OrderManager_moveOrder, (*logger_));
    }
  }

  /// 取得指定品种的买卖策略订单。
  auto getOMOrderSideHashMap(TickerId ticker_id) const {
    return &(ticker_side_order_.at(ticker_id));
  }

  /// 禁止默认构造、复制和移动，避免重复拥有资源。
  OrderManager() = delete;

  OrderManager(const OrderManager&) = delete;

  OrderManager(const OrderManager&&) = delete;

  OrderManager& operator=(const OrderManager&) = delete;

  OrderManager& operator=(const OrderManager&&) = delete;

 private:
  /// 所属交易引擎，用于发送客户请求。
  TradeEngine* trade_engine_ = nullptr;

  /// 报单前执行风险检查。
  const RiskManager& risk_manager_;

  std::string time_str_;
  Common::Logger* logger_ = nullptr;

  /// 先按品种、再按方向索引的策略订单数组。
  OMOrderTickerSideHashMap ticker_side_order_;

  /// 为新订单分配客户端订单 ID。
  OrderId next_order_id_ = 1;
};
}
