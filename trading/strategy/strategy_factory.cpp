#include "strategy_factory.h"

#include <stdexcept>
#include "market_maker.h"
#include "liquidity_taker.h"

namespace Trading {
namespace {
// RANDOM 是主线程产生请求的模拟负载，此处只保留原来的事件日志。
class PassiveStrategy final : public Strategy {
 public:
  explicit PassiveStrategy(Common::Logger* logger) : logger_(logger) {}

  void onOrderBookUpdate(Common::TickerId ticker, Common::Price price, Common::Side side,
                         const MarketOrderBook*) noexcept override {
    logger_->log("%:% %() % ticker:% price:% side:%\n", __FILE__, __LINE__, __FUNCTION__,
                 Common::getCurrentTimeStr(), ticker, Common::priceToString(price).c_str(),
                 Common::sideToString(side).c_str());
  }

  void onTradeUpdate(const Exchange::MEMarketUpdate* update, MarketOrderBook*) noexcept override {
    logger_->log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(),
                 update->toString().c_str());
  }

  void onOrderUpdate(const Exchange::MEClientResponse* response) noexcept override {
    logger_->log("%:% %() % %\n", __FILE__, __LINE__, __FUNCTION__, Common::getCurrentTimeStr(),
                 response->toString().c_str());
  }

 private:
  Common::Logger* logger_;
};
}

std::unique_ptr<Strategy> createStrategy(Common::AlgoType type, Common::Logger* logger,
                                         const FeatureEngine* features, OrderManager* orders,
                                         const Common::TradeEngineCfgHashMap& config) {
  switch (type) {
    case Common::AlgoType::MAKER:
      return std::make_unique<MarketMaker>(logger, features, orders, config);
    case Common::AlgoType::TAKER:
      return std::make_unique<LiquidityTaker>(logger, features, orders, config);
    case Common::AlgoType::RANDOM:
      return std::make_unique<PassiveStrategy>(logger);
    default:
      throw std::invalid_argument("不支持的策略类型");
  }
}
}
