#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include "trading/strategy/trade_engine.h"
#include "common/book_hash.h"

int main(int argc, char** argv) {
  using T = Exchange::MarketUpdateType;
  using E = Exchange::MEMarketUpdate;
  if (argc != 2)
    return 2;
  const size_t count = std::stoul(argv[1]);
  Exchange::ClientRequestLFQueue requests(2048);
  Exchange::ClientResponseLFQueue responses(2048);
  Exchange::MEMarketUpdateLFQueue market(2048);
  Common::TradeEngineCfgHashMap cfg{};
  Trading::TradeEngine engine(1, Common::AlgoType::RANDOM, cfg, &requests, &responses, &market);
  std::vector<E> orders;
  for (Common::TickerId i = 0; i < 8; ++i)
    market.try_push({T::CLEAR, Common::OrderId_INVALID, i});
  for (size_t i = 1; i <= count; ++i) {
    E e{T::ADD, i, Common::TickerId(i % 8), Common::Side::BUY, 100, 2, i};
    orders.push_back(e);
    market.try_push(e);
  }
  engine.run();
  E marker;
  marker.type_ = T::RECOVERY_COMMIT;
  marker.order_id_ = 42;
  marker.priority_ = engine.marketGeneration().load();
#ifndef TRADE_SIMPLE_RECOVERY
  marker.state_hash_ = Common::bookHash(orders);
#endif
  std::vector<uint64_t> samples;
  uint64_t checksum = 0;
  for (size_t i = 0; i < 1100; ++i) {
    engine.setMarketTrusted(false);
    if (!market.try_push(marker))
      throw std::runtime_error("queue full");
    const auto begin = std::chrono::steady_clock::now();
    engine.run();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - begin)
                        .count();
#ifdef TRADE_SIMPLE_RECOVERY
    if (!engine.marketReady())
      throw std::runtime_error("commit failed");
#else
    if (!engine.marketTrust().load())
      throw std::runtime_error("commit failed");
#endif
    if (i >= 100) {
      samples.push_back(ns);
      ++checksum;
    }
  }
  std::sort(samples.begin(), samples.end());
  std::cout << "{\"mode\":\"engine_commit_" << count
            << "\",\"p50_ns\":" << samples[samples.size() / 2]
            << ",\"p99_ns\":" << samples[samples.size() * 99 / 100]
            << ",\"samples\":1000,\"checksum\":" << checksum << "}\n";
}
