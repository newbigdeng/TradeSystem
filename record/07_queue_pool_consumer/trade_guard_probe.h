#pragma once

// 先加载标准库，再只在本测试翻译单元开放内部状态。
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#define private public
#include "trading/strategy/trade_engine.h"
#undef private

namespace TradeGuardProbe {
using namespace Common;
using namespace Exchange;
using namespace Trading;

constexpr TickerId Ticker = 2;
constexpr OrderId AskOrder = 1, BidOrder = 17;

inline void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

// 回调只读取 BBO 并累计结果，不报单；业务代码仍走真实虚函数分派。
class Observer final : public Strategy {
 public:
  bool recording = false, valid = true;
  uint64_t callbacks = 0, trades = 0, responses = 0, checksum = 0;

  void onOrderBookUpdate(TickerId ticker, Price price, Side side,
                         const MarketOrderBook* book) noexcept override {
    if (!recording)
      return;
    const auto* bbo = book->getBBO();
    const auto step = callbacks % 3;
    const auto bid_qty = step == 0 ? BookQty(5) : BookQty(3);
    valid = valid && ticker == Ticker && price == 100 && side == Side::BUY &&
            bbo->ask_price_ == 110 && bbo->ask_qty_ == 7 &&
            (step == 2 ? bbo->bid_price_ == Price_INVALID && bbo->bid_qty_ == BookQty_INVALID
                       : bbo->bid_price_ == 100 && bbo->bid_qty_ == bid_qty);
    checksum += 117 + (step == 2 ? 0 : uint64_t(bbo->bid_price_) + bbo->bid_qty_);
    ++callbacks;
  }

  void onTradeUpdate(const MEMarketUpdate*, MarketOrderBook*) noexcept override { ++trades; }
  void onOrderUpdate(const MEClientResponse*) noexcept override { ++responses; }
};

inline void drain(Logger& logger) {
  // 等待只在计时区外；这里只等诊断队列为空，不把它称为落盘确认。
  while (logger.queue_.size())
    std::this_thread::yield();
  require(logger.dropped_records() == 0, "diagnostic record dropped");
}

struct Fixture {
  ClientRequestLFQueue requests{4};
  ClientResponseLFQueue responses{4};
  MEMarketUpdateLFQueue updates{128};
  Observer* observer = nullptr;
  std::unique_ptr<TradeEngine> engine;

  Fixture() {
    auto strategy = std::make_unique<Observer>();
    observer = strategy.get();
    TradeEngineCfgHashMap config{};
    engine = std::make_unique<TradeEngine>(7, AlgoType::RANDOM, config, &requests, &responses,
                                         &updates, std::move(strategy));
    drain(engine->logger_);
  }

  MarketOrderBook& book() const { return *engine->ticker_order_book_[Ticker]; }

  void consume(const MEMarketUpdate& event) {
    require(updates.try_push(event), "input setup full");
    engine->run();
    drain(engine->logger_);
    require(!updates.peek() && !requests.peek() && !responses.peek(), "queues not empty");
  }

  void trust() {
    MEMarketUpdate marker;
    marker.type_ = MarketUpdateType::RECOVERY_COMMIT;
    marker.priority_ = engine->marketGeneration().load();
    consume(marker);  // 默认 ticker 无效；恢复标记必须先于 ticker 检查处理。
    require(engine->marketReady(), "current recovery marker did not enable market");
  }

  void seed() {
    consume({MarketUpdateType::ADD, AskOrder, Ticker, Side::SELL, 110, 7, 1});
    observer->recording = true;
  }

  void enqueueCycle() {
    require(updates.try_push({MarketUpdateType::ADD, BidOrder, Ticker, Side::BUY, 100, 5, 9}) &&
                updates.try_push({MarketUpdateType::MODIFY, BidOrder, Ticker, Side::BUY, 100, 3, 9}) &&
                updates.try_push({MarketUpdateType::CANCEL, BidOrder, Ticker, Side::BUY, 100, 3, 9}),
            "cycle setup full");
  }

  void validateEnd(size_t initial_order_free, size_t initial_price_free,
                   uint64_t generation, uint64_t cycles) const {
    const auto* bbo = book().getBBO();
    require(!updates.peek() && !responses.peek() && !requests.peek(), "consumer left messages");
    require(!book().oid_to_order_[BidOrder] && book().oid_to_order_[AskOrder] &&
                book().live_order_ids_.size() == 1 &&
                book().order_pool_.available() == initial_order_free &&
                book().orders_at_price_pool_.available() == initial_price_free &&
                bbo->bid_price_ == Price_INVALID && bbo->bid_qty_ == BookQty_INVALID &&
                bbo->ask_price_ == 110 && bbo->ask_qty_ == 7,
            "book, BBO or pool did not return to seeded state");
    require(engine->marketGeneration().load() == generation && engine->marketReady() &&
                engine->applied_generation_.load() == generation &&
                engine->positionInfo(Ticker)->position_ == 0 &&
                engine->positionInfo(Ticker)->volume_ == 0,
            "legal events changed generation, trust or position");
    require(observer->valid && observer->callbacks == cycles * 3 &&
                observer->checksum == cycles * 559 && observer->trades == 0 &&
                observer->responses == 0,
            "real callback order, BBO, checksum or count differs");
    require(std::isnan(engine->feature_engine_.getMktPrice(Ticker)) &&
                std::isnan(engine->feature_engine_.getAggTradeQtyRatio(Ticker)),
            "one-sided final book retained a valid feature");
  }
};

inline void validateFooter() {
  std::ifstream file("trading_engine_7.log");
  std::string line, last;
  while (std::getline(file, line))
    last = line;
  require(last == "[LOGGER] dropped_records=0", "final diagnostic footer missing or nonzero");
}
}
