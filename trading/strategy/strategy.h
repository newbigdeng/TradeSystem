#pragma once

#include "common/types.h"

namespace Exchange {
struct MEMarketUpdate;
struct MEClientResponse;
}

namespace Trading {
class MarketOrderBook;

// 策略只处理业务事件；行情恢复、持仓和报单风控仍由交易引擎统一管理。
class Strategy {
 public:
  virtual ~Strategy() = default;

  virtual void onOrderBookUpdate(Common::TickerId ticker, Common::Price price, Common::Side side,
                                 const MarketOrderBook* book) noexcept = 0;
  virtual void onTradeUpdate(const Exchange::MEMarketUpdate* update,
                             MarketOrderBook* book) noexcept = 0;
  virtual void onOrderUpdate(const Exchange::MEClientResponse* response) noexcept = 0;
};
}
