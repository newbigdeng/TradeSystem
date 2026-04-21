#pragma once

#include "common/macros.h"
#include "common/logging.h"

#include "exchange/order_server/client_response.h"

#include "om_order.h"
#include "risk_manager.h"

using namespace Common;

namespace Trading {
  class TradeEngine;

  /// 管理交易算法的订单，隐藏订单管理的复杂性以简化交易策略。
  class OrderManager {
  public:
    OrderManager(Common::Logger *logger, TradeEngine *trade_engine, RiskManager& risk_manager)
        : trade_engine_(trade_engine), risk_manager_(risk_manager), logger_(logger) {
    }

    /// 处理由客户端响应的订单更新并更新被管理订单的状态。
    auto onOrderUpdate(const Exchange::MEClientResponse *client_response) noexcept -> void {
      logger_->log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                   client_response->toString().c_str());
      auto order = &(ticker_side_order_.at(client_response->ticker_id_).at(sideToIndex(client_response->side_)));
      logger_->log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                   order->toString().c_str());

      switch (client_response->type_) {
        case Exchange::ClientResponseType::ACCEPTED: {
          order->order_state_ = OMOrderState::LIVE;
        }
          break;
        case Exchange::ClientResponseType::CANCELED: {
          order->order_state_ = OMOrderState::DEAD;
        }
          break;
        case Exchange::ClientResponseType::FILLED: {
          order->qty_ = client_response->leaves_qty_;
          if(!order->qty_)
            order->order_state_ = OMOrderState::DEAD;
        }
          break;
        case Exchange::ClientResponseType::CANCEL_REJECTED:
        case Exchange::ClientResponseType::INVALID: {
        }
          break;
      }
    }

    /// 发送具有指定属性的新订单，并更新此处传递的OMOrder对象。
    auto newOrder(OMOrder *order, TickerId ticker_id, Price price, Side side, Qty qty) noexcept -> void;

    /// 发送指定订单的取消，并更新此处传递的OMOrder对象。
    auto cancelOrder(OMOrder *order) noexcept -> void;

    /// 移动指定一侧的单个订单，使其具有指定的价格和数量。
    /// 这将在发送订单之前执行风险检查，并更新此处传递的OMOrder对象。
    auto moveOrder(OMOrder *order, TickerId ticker_id, Price price, Side side, Qty qty) noexcept {
      switch (order->order_state_) {
        case OMOrderState::LIVE: {
          if(order->price_ != price) {
            START_MEASURE(Trading_OrderManager_cancelOrder);
            cancelOrder(order);
            END_MEASURE(Trading_OrderManager_cancelOrder, (*logger_));
          }
        }
          break;
        case OMOrderState::INVALID:
        case OMOrderState::DEAD: {
          if(LIKELY(price != Price_INVALID)) {
            START_MEASURE(Trading_RiskManager_checkPreTradeRisk);
            const auto risk_result = risk_manager_.checkPreTradeRisk(ticker_id, side, qty);
            END_MEASURE(Trading_RiskManager_checkPreTradeRisk, (*logger_));
            if(LIKELY(risk_result == RiskCheckResult::ALLOWED)) {
              START_MEASURE(Trading_OrderManager_newOrder);
              newOrder(order, ticker_id, price, side, qty);
              END_MEASURE(Trading_OrderManager_newOrder, (*logger_));
            } else
              logger_->log("%:% %() % Ticker:% Side:% Qty:% RiskCheckResult:%\n", __FILE__, __LINE__, __FUNCTION__,
                           Common::getCurrentTimeStr(&time_str_),
                           tickerIdToString(ticker_id), sideToString(side), qtyToString(qty),
                           riskCheckResultToString(risk_result));
          }
        }
          break;
        case OMOrderState::PENDING_NEW:
        case OMOrderState::PENDING_CANCEL:
          break;
      }
    }

    /// 使数量为clip的订单位于指定的买入和卖出价格。
    /// 如果没有订单，这可能导致发送新订单。
    /// 如果现有订单不在指定价格或不是指定数量，这可能导致取消现有订单。
    /// 为买入或卖出价格指定Price_INVALID表示我们不希望在那里有订单。
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

    /// 辅助方法，获取指定交易品种ID的买入和卖出OMOrder。
    auto getOMOrderSideHashMap(TickerId ticker_id) const {
      return &(ticker_side_order_.at(ticker_id));
    }

    /// 删除默认、复制和移动构造函数及赋值操作符。
    OrderManager() = delete;
    OrderManager(const OrderManager &) = delete;
    OrderManager(const OrderManager &&) = delete;
    OrderManager &operator=(const OrderManager &) = delete;
    OrderManager &operator=(const OrderManager &&) = delete;
  private:
    /// 父交易引擎对象，用于发送客户端请求。
    TradeEngine *trade_engine_ = nullptr;

    /// 风险管理器，执行交易前风险检查。
    const RiskManager& risk_manager_;

    std::string time_str_;
    Common::Logger *logger_ = nullptr;

    /// Hash map container from TickerId -> Side -> OMOrder.
    OMOrderTickerSideHashMap ticker_side_order_;

    /// Used to set OrderIds on outgoing new order requests.
    OrderId next_order_id_ = 1;
  };
}