#pragma once

#include <cstdint>
#include <functional>
#include <memory>

#include "common/types.h"
#include "exchange/market_data/market_update.h"
#include "exchange/order_server/client_response.h"

namespace Trading {
class MarketOrderBook;
}

namespace Audit {
// 同一份小计算供两种分派方式调用，避免把不同业务体的成本算作多态开销。
struct Payload {
  uint64_t total = 0;

  void book(Common::TickerId ticker, Common::Price price, Common::Side side,
            const Trading::MarketOrderBook*) noexcept {
    total += ticker + static_cast<uint64_t>(price) + static_cast<int8_t>(side);
  }

  void trade(const Exchange::MEMarketUpdate* update, Trading::MarketOrderBook*) noexcept {
    total += update->qty_ + update->order_id_;
  }

  void order(const Exchange::MEClientResponse* response) noexcept {
    total += response->leaves_qty_ + response->response_id_;
  }
};

struct Callbacks {
  std::function<void(Common::TickerId, Common::Price, Common::Side,
                     const Trading::MarketOrderBook*)>
      book;
  std::function<void(const Exchange::MEMarketUpdate*, Trading::MarketOrderBook*)> trade;
  std::function<void(const Exchange::MEClientResponse*)> order;
};

struct VirtualStrategy {
  virtual ~VirtualStrategy() = default;
  virtual void book(Common::TickerId, Common::Price, Common::Side,
                    const Trading::MarketOrderBook*) noexcept = 0;
  virtual void trade(const Exchange::MEMarketUpdate*, Trading::MarketOrderBook*) noexcept = 0;
  virtual void order(const Exchange::MEClientResponse*) noexcept = 0;
};

Callbacks callbacks(Payload&);
std::unique_ptr<VirtualStrategy> strategy(Payload&);
}
