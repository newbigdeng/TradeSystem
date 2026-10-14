#include "dispatch_benchmark.h"

namespace Audit {
Callbacks callbacks(Payload& payload) {
  return {[&payload](auto ticker, auto price, auto side, auto* book) {
            payload.book(ticker, price, side, book);
          },
          [&payload](auto* event, auto* book) { payload.trade(event, book); },
          [&payload](auto* response) { payload.order(response); }};
}

// 工厂放在单独翻译单元，测量循环看不到具体类型；关闭 LTO 后不能去虚化。
class Concrete final : public VirtualStrategy {
 public:
  explicit Concrete(Payload& payload) : payload_(payload) {}

  void book(Common::TickerId ticker, Common::Price price, Common::Side side,
            const Trading::MarketOrderBook* book) noexcept override {
    payload_.book(ticker, price, side, book);
  }

  void trade(const Exchange::MEMarketUpdate* event,
             Trading::MarketOrderBook* book) noexcept override {
    payload_.trade(event, book);
  }

  void order(const Exchange::MEClientResponse* response) noexcept override {
    payload_.order(response);
  }

 private:
  Payload& payload_;
};

std::unique_ptr<VirtualStrategy> strategy(Payload& payload) {
  return std::make_unique<Concrete>(payload);
}
}
