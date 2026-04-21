#pragma once

#include "common/macros.h"
#include "common/logging.h"

#include "order_manager.h"
#include "feature_engine.h"

using namespace Common;

namespace Trading {
  class LiquidityTaker {
  public:
    LiquidityTaker(Common::Logger *logger, TradeEngine *trade_engine, const FeatureEngine *feature_engine,
                   OrderManager *order_manager,
                   const TradeEngineCfgHashMap &ticker_cfg);

    /// 处理订单簿更新，对于流动性获取算法来说没有实际处理。
    auto onOrderBookUpdate(TickerId ticker_id, Price price, Side side, MarketOrderBook *) noexcept -> void {
      logger_->log("%:% %() % ticker:% price:% side:%\n", __FILE__, __LINE__, __FUNCTION__,
                   Common::getCurrentTimeStr(&time_str_), ticker_id, Common::priceToString(price).c_str(),
                   Common::sideToString(side).c_str());
    }

    /// 处理交易事件，从特征引擎获取激进交易比率，对照交易阈值检查并发送激进订单。
    auto onTradeUpdate(const Exchange::MEMarketUpdate *market_update, MarketOrderBook *book) noexcept -> void {
      logger_->log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                   market_update->toString().c_str());

      const auto bbo = book->getBBO();
      const auto agg_qty_ratio = feature_engine_->getAggTradeQtyRatio();

      if (LIKELY(bbo->bid_price_ != Price_INVALID && bbo->ask_price_ != Price_INVALID && agg_qty_ratio != Feature_INVALID)) {
        logger_->log("%:% %() % % agg-qty-ratio:%\n", __FILE__, __LINE__, __FUNCTION__,
                     Common::getCurrentTimeStr(&time_str_),
                     bbo->toString().c_str(), agg_qty_ratio);

        const auto clip = ticker_cfg_.at(market_update->ticker_id_).clip_;
        const auto threshold = ticker_cfg_.at(market_update->ticker_id_).threshold_;

        if (agg_qty_ratio >= threshold) {
          START_MEASURE(Trading_OrderManager_moveOrders);
          if (market_update->side_ == Side::BUY)
            order_manager_->moveOrders(market_update->ticker_id_, bbo->ask_price_, Price_INVALID, clip);
          else
            order_manager_->moveOrders(market_update->ticker_id_, Price_INVALID, bbo->bid_price_, clip);
          END_MEASURE(Trading_OrderManager_moveOrders, (*logger_));
        }
      }
    }

    /// 处理策略订单的客户端响应。
    auto onOrderUpdate(const Exchange::MEClientResponse *client_response) noexcept -> void {
      logger_->log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(&time_str_),
                   client_response->toString().c_str());
      START_MEASURE(Trading_OrderManager_onOrderUpdate);
      order_manager_->onOrderUpdate(client_response);
      END_MEASURE(Trading_OrderManager_onOrderUpdate, (*logger_));
    }

    /// 删除默认、复制和移动构造函数及赋值操作符。
    LiquidityTaker() = delete;

    LiquidityTaker(const LiquidityTaker &) = delete;

    LiquidityTaker(const LiquidityTaker &&) = delete;

    LiquidityTaker &operator=(const LiquidityTaker &) = delete;

    LiquidityTaker &operator=(const LiquidityTaker &&) = delete;

  private:
    /// 驱动流动性获取算法的特征引擎。
    const FeatureEngine *feature_engine_ = nullptr;

    /// 流动性获取算法用于发送激进订单。
    OrderManager *order_manager_ = nullptr;

    std::string time_str_;
    Common::Logger *logger_ = nullptr;

    /// 保存流动性获取算法的交易配置。
    const TradeEngineCfgHashMap ticker_cfg_;
  };
}