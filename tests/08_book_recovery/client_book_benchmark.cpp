#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include "trading/strategy/market_order_book.h"

using Clock = std::chrono::steady_clock;
using E = Exchange::MEMarketUpdate;
using T = Exchange::MarketUpdateType;

void require(bool ok) {
  if (!ok)
    throw std::runtime_error("client book benchmark differs");
}

int main(int argc, char** argv) {
  try {
    require(argc == 2);
    const std::string mode = argv[1];
    require(mode == "best" || mode == "worst" || mode == "random");
    // 测格式化与入队成本；独立测量关闭磁盘写入，扩大队列避免“丢弃/保留”工作量不同。
    Common::Logger logger("/dev/null", 1 << 18);
    auto book = std::make_unique<Trading::MarketOrderBook>(0, &logger);
    constexpr size_t depth = 96, measured = 300, warmup = 30;
    std::vector<size_t> ranks(depth);
    std::iota(ranks.begin(), ranks.end(), 0);
    std::mt19937 rng(12345);
    if (mode == "random")
      std::shuffle(ranks.begin(), ranks.end(), rng);
    std::vector<uint64_t> samples;
    uint64_t next = 1, accepted = 0;
    for (size_t round = 0; round < measured + warmup; ++round) {
      const auto first = next;
      const auto start = Clock::now();
      for (auto rank : ranks) {
        const auto delta = mode == "best" ? int64_t(rank) : -int64_t(rank);
        E buy{T::ADD, next++, 0, Common::Side::BUY, 1000 + delta, 1, 1};
        E sell{T::ADD, next++, 0, Common::Side::SELL, 5000 - delta, 1, 1};
        require(book->onMarketUpdate(&buy));
        require(book->onMarketUpdate(&sell));
        accepted += 2;
      }
      const auto ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
      if (round >= warmup)
        samples.push_back(ns / (2 * depth));
      require(book->liveOrders().size() == depth * 2);
      require(book->getBBO()->bid_price_ == (mode == "best" ? 1095 : 1000));
      require(book->getBBO()->ask_price_ == (mode == "best" ? 4905 : 5000));
      for (auto id = first; id < next; ++id) {
        const auto index = (id - first) / 2;
        const auto delta = mode == "best" ? int64_t(ranks[index]) : -int64_t(ranks[index]);
        E cancel{T::CANCEL,
                 id,
                 0,
                 (id - first) % 2 ? Common::Side::SELL : Common::Side::BUY,
                 (id - first) % 2 ? 5000 - delta : 1000 + delta,
                 1,
                 1};
        require(book->onMarketUpdate(&cancel));
      }
      require(book->liveOrders().empty());
    }
    std::sort(samples.begin(), samples.end());
    std::cout << "{\"mode\":\"" << mode << "\",\"p50_ns\":" << samples[samples.size() / 2]
              << ",\"p99_ns\":" << samples[samples.size() * 99 / 100]
              << ",\"samples\":" << samples.size() << ",\"accepted\":" << accepted
              << ",\"dropped_logs\":" << logger.dropped_records() << "}\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
