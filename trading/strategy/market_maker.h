#pragma once

#include "common/macros.h"
#include "common/logging.h"

#include "order_manager.h"
#include "feature_engine.h"
#include "strategy.h"

using namespace Common;

namespace Trading {
class MarketMaker final : public Strategy {
 public:
  MarketMaker(Common::Logger* logger, const FeatureEngine* feature_engine,
              OrderManager* order_manager, const TradeEngineCfgHashMap& ticker_cfg);

  /// 根据公允价格和策略阈值调整被动挂单。
  auto onOrderBookUpdate(TickerId ticker_id, Price price, Side side,
                         const MarketOrderBook* book) noexcept -> void override {
    logger_->log("%:% %() % ticker:% price:% side:%\n", __FILE__, __LINE__, __FUNCTION__,
                 Common::getCurrentTimeStr(), ticker_id, Common::priceToString(price).c_str(),
                 Common::sideToString(side).c_str());

    const auto bbo = book->getBBO();
    const auto fair_price = feature_engine_->getMktPrice(ticker_id);

    if (LIKELY(bbo->bid_price_ != Price_INVALID && bbo->ask_price_ != Price_INVALID &&
               std::isfinite(fair_price))) {
      logger_->log("%:% %() % % fair-price:%\n", __FILE__, __LINE__, __FUNCTION__,
                   Common::getCurrentTimeStr(), bbo->toString().c_str(), fair_price);

      const auto clip = ticker_cfg_.at(ticker_id).clip_;
      const auto threshold = ticker_cfg_.at(ticker_id).threshold_;

      const auto bid_price = bbo->bid_price_ - (fair_price - bbo->bid_price_ >= threshold ? 0 : 1);
      if (bbo->bid_price_ <= 1 || bbo->ask_price_ >= Price_INVALID - 1)
        return;
      const auto ask_price = bbo->ask_price_ + (bbo->ask_price_ - fair_price >= threshold ? 0 : 1);

      START_MEASURE(Trading_OrderManager_moveOrders);
      order_manager_->moveOrders(ticker_id, bid_price, ask_price, clip);
      END_MEASURE(Trading_OrderManager_moveOrders, (*logger_));
    }
  }

  /// 做市策略不响应成交通知。
  auto onTradeUpdate(const Exchange::MEMarketUpdate* market_update,
                     MarketOrderBook* /*订单簿参数*/) noexcept -> void override {
    logger_->log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(),
                 market_update->toString().c_str());
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
  MarketMaker() = delete;

  MarketMaker(const MarketMaker&) = delete;

  MarketMaker(const MarketMaker&&) = delete;

  MarketMaker& operator=(const MarketMaker&) = delete;

  MarketMaker& operator=(const MarketMaker&&) = delete;

 private:
  /// 提供做市策略所需特征。
  const FeatureEngine* feature_engine_ = nullptr;

  /// 负责管理做市策略的被动挂单。
  OrderManager* order_manager_ = nullptr;

  std::string time_str_;
  Common::Logger* logger_ = nullptr;

  /// 做市策略配置。
  const TradeEngineCfgHashMap ticker_cfg_;
};
}
