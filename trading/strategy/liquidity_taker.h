#pragma once

#include "common/macros.h"
#include "common/logging.h"

#include "order_manager.h"
#include "feature_engine.h"
#include "strategy.h"

using namespace Common;

namespace Trading {
class LiquidityTaker final : public Strategy {
 public:
  LiquidityTaker(Common::Logger* logger, const FeatureEngine* feature_engine,
                 OrderManager* order_manager, const TradeEngineCfgHashMap& ticker_cfg);

  /// 吃单策略不响应订单簿更新。
  auto onOrderBookUpdate(TickerId ticker_id, Price price, Side side,
                         const MarketOrderBook*) noexcept -> void override {
    logger_->log("%:% %() % ticker:% price:% side:%\n", __FILE__, __LINE__, __FUNCTION__,
                 Common::getCurrentTimeStr(), ticker_id, Common::priceToString(price).c_str(),
                 Common::sideToString(side).c_str());
  }

  /// 根据主动成交比例和策略阈值决定是否发送吃单。
  auto onTradeUpdate(const Exchange::MEMarketUpdate* market_update, MarketOrderBook* book) noexcept
      -> void override {
    logger_->log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(),
                 market_update->toString().c_str());

    const auto bbo = book->getBBO();
    const auto agg_qty_ratio = feature_engine_->getAggTradeQtyRatio(market_update->ticker_id_);

    if (LIKELY(bbo->bid_price_ != Price_INVALID && bbo->ask_price_ != Price_INVALID &&
               std::isfinite(agg_qty_ratio))) {
      logger_->log("%:% %() % % agg-qty-ratio:%\n", __FILE__, __LINE__, __FUNCTION__,
                   Common::getCurrentTimeStr(), bbo->toString().c_str(), agg_qty_ratio);

      const auto clip = ticker_cfg_.at(market_update->ticker_id_).clip_;
      const auto threshold = ticker_cfg_.at(market_update->ticker_id_).threshold_;

      if (agg_qty_ratio >= threshold) {
        START_MEASURE(Trading_OrderManager_moveOrders);
        if (market_update->side_ == Side::BUY)
          order_manager_->moveOrders(market_update->ticker_id_, bbo->ask_price_, Price_INVALID,
                                     clip);
        else
          order_manager_->moveOrders(market_update->ticker_id_, Price_INVALID, bbo->bid_price_,
                                     clip);
        END_MEASURE(Trading_OrderManager_moveOrders, (*logger_));
      }
    }
  }

  /// 处理策略订单的客户回报。
  auto onOrderUpdate(const Exchange::MEClientResponse* client_response) noexcept -> void override {
    logger_->log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(),
                 client_response->toString().c_str());
    START_MEASURE(Trading_OrderManager_onOrderUpdate);
    order_manager_->onOrderUpdate(client_response);
    END_MEASURE(Trading_OrderManager_onOrderUpdate, (*logger_));
  }

  /// 禁止默认构造、复制和移动，避免重复拥有资源。
  LiquidityTaker() = delete;

  LiquidityTaker(const LiquidityTaker&) = delete;

  LiquidityTaker(const LiquidityTaker&&) = delete;

  LiquidityTaker& operator=(const LiquidityTaker&) = delete;

  LiquidityTaker& operator=(const LiquidityTaker&&) = delete;

 private:
  /// 提供吃单策略所需特征。
  const FeatureEngine* feature_engine_ = nullptr;

  /// 负责发送和管理吃单策略的订单。
  OrderManager* order_manager_ = nullptr;

  std::string time_str_;
  Common::Logger* logger_ = nullptr;

  /// 吃单策略配置。
  const TradeEngineCfgHashMap ticker_cfg_;
};
}
