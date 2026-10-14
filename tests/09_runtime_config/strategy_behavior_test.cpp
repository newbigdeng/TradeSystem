#include "strategy/trade_engine.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Common::AlgoType;
using Common::Side;
using Exchange::ClientRequestType;
using Exchange::ClientResponseType;
using Exchange::MEClientRequest;

void check(bool result, const char* message) {
  if (!result)
    throw std::runtime_error(message);
}

struct Fixture {
  Exchange::ClientRequestLFQueue requests{64};
  Exchange::ClientResponseLFQueue responses{64};
  Exchange::MEMarketUpdateLFQueue market{64};
  Common::TradeEngineCfgHashMap cfg{};
  std::unique_ptr<Trading::TradeEngine> engine;
  Common::Logger logger{"strategy_behavior_book.log"};
  std::unique_ptr<Trading::MarketOrderBook> book;
  uint64_t response_id = 1;

  explicit Fixture(AlgoType algo, Common::Qty clip = 3) {
    for (auto& entry : cfg)
      entry = {clip, 0.5, {100, 1000, -1000000}};
    engine = std::make_unique<Trading::TradeEngine>(1, algo, cfg, &requests, &responses, &market);
    book = std::make_unique<Trading::MarketOrderBook>(0, &logger);
    *const_cast<Trading::BBO*>(book->getBBO()) = {100, 102, 10, 10};
  }

  ~Fixture() {
    // 持仓模块借用测试订单簿的 BBO；引擎停机输出持仓前，订单簿必须仍然存活。
    engine.reset();
  }

  std::vector<MEClientRequest> drain() {
    std::vector<MEClientRequest> output;
    MEClientRequest event;
    while (requests.try_pop(event))
      output.push_back(event);
    return output;
  }

  void bookUpdate() {
    engine->onOrderBookUpdate(0, book->getBBO()->bid_price_, Side::BUY, book.get());
  }

  void trade(Side side, Common::Qty qty) {
    Exchange::MEMarketUpdate event{Exchange::MarketUpdateType::TRADE, 0, 0, side, 101, qty, 0};
    engine->onTradeUpdate(&event, book.get());
  }

  void respond(const MEClientRequest& request, ClientResponseType type) {
    Exchange::MEClientResponse response{
        type,           1, request.ticker_id_, request.order_id_, request.order_id_, request.side_,
        request.price_, 0, request.qty_};
    response.response_id_ = response_id++;
    engine->onOrderUpdate(&response);
  }
};

void newRequest(const MEClientRequest& request, Side side, Common::Price price, Common::Qty qty) {
  check(request.type_ == ClientRequestType::NEW && request.client_id_ == 1 &&
            request.ticker_id_ == 0 && request.order_id_ > 0 && request.side_ == side &&
            request.price_ == price && request.qty_ == qty,
        "strategy generated wrong request");
}

void maker() {
  Fixture f(AlgoType::MAKER);
  f.bookUpdate();
  check(f.drain().empty(), "MAKER ran before recovery completed");
  f.engine->setMarketTrusted(true);
  f.bookUpdate();
  const auto first = f.drain();
  check(first.size() == 2, "MAKER did not quote both sides");
  newRequest(first[0], Side::BUY, 100, 3);
  newRequest(first[1], Side::SELL, 102, 3);
  for (const auto& request : first)
    f.respond(request, ClientResponseType::ACCEPTED);
  f.bookUpdate();
  check(f.drain().empty(), "MAKER resent unchanged live quotes");
  *const_cast<Trading::BBO*>(f.book->getBBO()) = {101, 103, 10, 10};
  f.bookUpdate();
  const auto canceled = f.drain();
  check(canceled.size() == 2, "MAKER did not cancel displaced quotes");
  for (size_t i = 0; i < canceled.size(); ++i) {
    check(canceled[i].type_ == ClientRequestType::CANCEL &&
              canceled[i].order_id_ == first[i].order_id_,
          "MAKER canceled wrong order");
    f.respond(first[i], ClientResponseType::CANCELED);
  }
  f.bookUpdate();
  const auto replaced = f.drain();
  check(replaced.size() == 2, "MAKER failed to replace canceled quotes");
  newRequest(replaced[0], Side::BUY, 101, 3);
  newRequest(replaced[1], Side::SELL, 103, 3);
  f.engine->setMarketTrusted(false);
  *const_cast<Trading::BBO*>(f.book->getBBO()) = {102, 104, 10, 10};
  f.bookUpdate();
  check(f.drain().empty(), "stale market still drove MAKER");
}

void taker(Side side) {
  Fixture f(AlgoType::TAKER);
  f.trade(side, 5);
  check(f.drain().empty(), "TAKER ran before recovery completed");
  f.engine->setMarketTrusted(true);
  f.trade(side, 4);
  check(f.drain().empty(), "TAKER ignored threshold");
  f.trade(side, 5);
  const auto request = f.drain();
  check(request.size() == 1, "TAKER did not submit exactly one side");
  newRequest(request.front(), side, side == Side::BUY ? 102 : 100, 3);
  f.respond(request.front(), ClientResponseType::ACCEPTED);
  f.trade(side, 5);
  check(f.drain().empty(), "TAKER duplicated unchanged live order");
  f.engine->setMarketTrusted(false);
  f.trade(side == Side::BUY ? Side::SELL : Side::BUY, 5);
  check(f.drain().empty(), "stale market still drove TAKER");
}

void randomAndRisk() {
  {
    Fixture f(AlgoType::RANDOM);
    f.engine->setMarketTrusted(true);
    f.bookUpdate();
    f.trade(Side::BUY, 5);
    check(f.drain().empty(), "RANDOM acquired an event-driven trading behavior");
  }
  for (const auto type : {AlgoType::MAKER, AlgoType::TAKER}) {
    Fixture f(type, 101);
    f.engine->setMarketTrusted(true);
    f.bookUpdate();
    f.trade(Side::BUY, 5);
    check(f.drain().empty(), "strategy bypassed maximum-order-size gate");
  }
}

void benchmark(AlgoType type) {
  Fixture f(type);
  f.engine->setMarketTrusted(true);
  const auto call = [&] {
    if (type == AlgoType::MAKER)
      f.bookUpdate();
    else
      f.trade(Side::BUY, 5);
  };
  for (size_t i = 0; i < 100; ++i)
    call();
  const auto initial = f.drain();
  check(initial.size() == (type == AlgoType::MAKER ? 2 : 1), "benchmark initial request differs");
  constexpr size_t bursts = 200;
  constexpr size_t per_burst = 64;
  std::vector<double> samples;
  samples.reserve(bursts);
  for (size_t burst = 0; burst < bursts; ++burst) {
    const auto begin = std::chrono::steady_clock::now();
    for (size_t i = 0; i < per_burst; ++i)
      call();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - begin)
                        .count();
    samples.push_back(double(ns) / per_burst);
  }
  check(f.drain().empty(), "benchmark generated repeated pending requests");
  std::sort(samples.begin(), samples.end());
  std::cout << "{\"mode\":\"" << (type == AlgoType::MAKER ? "maker" : "taker")
            << "\",\"calls\":" << bursts * per_burst << ",\"p50_ns\":" << samples[100]
            << ",\"p99_ns\":" << samples[198] << ",\"initial_requests\":" << initial.size()
            << "}\n";
}
}

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "maker") {
      benchmark(AlgoType::MAKER);
    } else if (argc == 2 && std::string(argv[1]) == "taker") {
      benchmark(AlgoType::TAKER);
    } else if (argc == 1) {
      maker();
      taker(Side::BUY);
      taker(Side::SELL);
      randomAndRisk();
      std::cout
          << "PASS MAKER quote/accept/cancel/replace; TAKER threshold/buy/sell; stale/risk gates; RANDOM\n";
    } else {
      return 2;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
