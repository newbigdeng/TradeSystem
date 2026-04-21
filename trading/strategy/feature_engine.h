#pragma once

#include "common/macros.h"
#include "common/logging.h"

using namespace Common;

namespace Trading {
  /// 哨兵值，表示无效/未初始化的特征值。
  constexpr auto Feature_INVALID = std::numeric_limits<double>::quiet_NaN();

  class FeatureEngine {
  public:
    FeatureEngine(Common::Logger *logger)
        : logger_(logger) {
    }

    /// 处理订单簿的变化，在这种情况下计算公平市场价格。
    auto onOrderBookUpdate(TickerId ticker_id, Price price, Side side, MarketOrderBook* book) noexcept -> void {
      const auto bbo = book->getBBO();
      if(LIKELY(bbo->bid_price_ != Price_INVALID && bbo->ask_price_ != Price_INVALID)) {
        mkt_price_ = (bbo->bid_price_ * bbo->ask_qty_ + bbo->ask_price_ * bbo->bid_qty_) / static_cast<double>(bbo->bid_qty_ + bbo->ask_qty_);
      }

      logger_->log("%:% %() % ticker:% price:% side:% mkt-price:% agg-trade-ratio:%\n", __FILE__, __LINE__, __FUNCTION__,
                   Common::getCurrentTimeStr(&time_str_), ticker_id, Common::priceToString(price).c_str(),
                   Common::sideToString(side).c_str(), mkt_price_, agg_trade_qty_ratio_);
    }

    /// 处理交易事件，在这种情况下计算特征以捕获相对于BBO数量的激进交易数量比率。
    auto onTradeUpdate(const Exchange::MEMarketUpdate *market_update, MarketOrderBook* book) noexcept -> void {
      const auto bbo = book->getBBO();
      if(LIKELY(bbo->bid_price_ != Price_INVALID && bbo->ask_price_ != Price_INVALID)) {
        agg_trade_qty_ratio_ = static_cast<double>(market_update->qty_) / (market_update->side_ == Side::BUY ? bbo->ask_qty_ : bbo->bid_qty_);
      }

      logger_->log("%:% %() % % mkt-price:% agg-trade-ratio:%\n", __FILE__, __LINE__, __FUNCTION__,
                   Common::getCurrentTimeStr(&time_str_),
                   market_update->toString().c_str(), mkt_price_, agg_trade_qty_ratio_);
    }

    auto getMktPrice() const noexcept {
      return mkt_price_;
    }

    auto getAggTradeQtyRatio() const noexcept {
      return agg_trade_qty_ratio_;
    }

    /// 删除默认、复制和移动构造函数及赋值操作符。
    FeatureEngine() = delete;

    FeatureEngine(const FeatureEngine &) = delete;

    FeatureEngine(const FeatureEngine &&) = delete;

    FeatureEngine &operator=(const FeatureEngine &) = delete;

    FeatureEngine &operator=(const FeatureEngine &&) = delete;

  private:
    std::string time_str_;
    Common::Logger *logger_ = nullptr;

    /// 我们在特征引擎中计算的两个特征。
    double mkt_price_ = Feature_INVALID, agg_trade_qty_ratio_ = Feature_INVALID;
  };
}