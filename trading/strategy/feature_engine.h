#pragma once

#include <cmath>

#include "common/logging.h"
#include "market_order_book.h"

namespace Trading {

// 无效特征使用 NaN，策略需先检查 isfinite，不能把缺失报价当成零。
constexpr double Feature_INVALID = std::numeric_limits<double>::quiet_NaN();

class FeatureEngine {
 public:
  explicit FeatureEngine(Common::Logger* logger) : logger_(logger) {
    prices_.fill(Feature_INVALID);
    ratios_.fill(Feature_INVALID);
  }

  void onOrderBookUpdate(Common::TickerId ticker, Common::Price price, Common::Side side,
                         MarketOrderBook* book) noexcept {
    if (ticker >= Common::ME_MAX_TICKERS)
      return;

    const auto* bbo = book->getBBO();
    prices_[ticker] = Feature_INVALID;
    // 任一侧报价缺失时，旧成交比率也不能继续用于策略判断。
    if (valid(*bbo)) {
      const double denominator = double(bbo->bid_qty_) + double(bbo->ask_qty_);

      // 用对侧挂单量给价格加权：买量越大，估值越接近卖价；先转 double 避免整数乘法溢出。
      prices_[ticker] = (double(bbo->bid_price_) * double(bbo->ask_qty_) +
                         double(bbo->ask_price_) * double(bbo->bid_qty_)) /
                        denominator;
      if (!std::isfinite(prices_[ticker]))
        prices_[ticker] = Feature_INVALID;
    } else
      ratios_[ticker] = Feature_INVALID;

    logger_->log("FEATURE ticker:% price:% side:% fair:% ratio:%\n", ticker, price, int(side),
                 prices_[ticker], ratios_[ticker]);
  }

  void onTradeUpdate(const Exchange::MEMarketUpdate* event, MarketOrderBook* book) noexcept {
    if (event->ticker_id_ >= Common::ME_MAX_TICKERS)
      return;

    auto& ratio = ratios_[event->ticker_id_];
    ratio = Feature_INVALID;
    const auto* bbo = book->getBBO();
    if (valid(*bbo) && event->qty_ > 0 && event->qty_ != Common::Qty_INVALID &&
        (event->side_ == Common::Side::BUY || event->side_ == Common::Side::SELL)) {
      // TRADE.side_ 表示主动方；主动买对比卖一数量，主动卖对比买一数量。
      ratio = double(event->qty_) /
              double(event->side_ == Common::Side::BUY ? bbo->ask_qty_ : bbo->bid_qty_);
      if (!std::isfinite(ratio))
        ratio = Feature_INVALID;
    }

    logger_->log("TRADE FEATURE ticker:% ratio:%\n", event->ticker_id_, ratio);
  }

  double getMktPrice(Common::TickerId ticker) const noexcept {
    return ticker < Common::ME_MAX_TICKERS ? prices_[ticker] : Feature_INVALID;
  }

  double getAggTradeQtyRatio(Common::TickerId ticker) const noexcept {
    return ticker < Common::ME_MAX_TICKERS ? ratios_[ticker] : Feature_INVALID;
  }

  FeatureEngine(const FeatureEngine&) = delete;
  FeatureEngine& operator=(const FeatureEngine&) = delete;

 private:
  static bool valid(const BBO& bbo) noexcept {
    // 有效双边价格和正挂单量，既保证估值有意义，也保证两个公式的分母不为零。
    return bbo.bid_price_ > 0 && bbo.ask_price_ > 0 && bbo.bid_price_ != Common::Price_INVALID &&
           bbo.ask_price_ != Common::Price_INVALID && bbo.bid_qty_ > 0 && bbo.ask_qty_ > 0 &&
           bbo.bid_qty_ != BookQty_INVALID && bbo.ask_qty_ != BookQty_INVALID;
  }

  Common::Logger* logger_;
  std::array<double, Common::ME_MAX_TICKERS> prices_, ratios_;
};

}
