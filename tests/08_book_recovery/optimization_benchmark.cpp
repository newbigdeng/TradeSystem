#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include "exchange/matcher/me_order_book.h"
#include "trading/market_data/market_recovery.h"

using Clock = std::chrono::steady_clock;
using E = Exchange::MEMarketUpdate;
using U = Exchange::MDPMarketUpdate;
using T = Exchange::MarketUpdateType;

void require(bool ok) {
  if (!ok)
    throw std::runtime_error("benchmark result differs");
}

void result(const char* mode, std::vector<uint64_t> samples, uint64_t checksum) {
  std::sort(samples.begin(), samples.end());
  std::cout << "{\"mode\":\"" << mode << "\",\"p50_ns\":" << samples[samples.size() / 2]
            << ",\"p99_ns\":" << samples[samples.size() * 99 / 100]
            << ",\"samples\":" << samples.size() << ",\"checksum\":" << checksum << "}\n";
}

uint64_t elapsed(Clock::time_point begin) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
}

void book(const std::string& mode) {
  Common::Logger logger("book_benchmark.log");
  Exchange::MEOrderBook book(0, &logger, nullptr);
  uint64_t accepted = 0, updates = 0;
  book.response_sink_ = [&](const auto& r) {
    if (r.type_ == Exchange::ClientResponseType::ACCEPTED)
      ++accepted;
  };
  book.market_sink_ = [&](const auto& u) {
    if (u.type_ == T::ADD)
      ++updates;
  };
  constexpr size_t depth = 96, rounds = 300;
  std::vector<size_t> ranks(depth);
  std::iota(ranks.begin(), ranks.end(), 0);
  std::mt19937 rng(12345);
  if (mode == "book_random")
    std::shuffle(ranks.begin(), ranks.end(), rng);
  std::vector<uint64_t> samples;
  uint64_t next = 1;
  for (size_t round = 0; round < rounds + 30; ++round) {
    const auto first = next;
    const auto start = Clock::now();
    for (auto rank : ranks) {
      const auto delta = mode == "book_best" ? int64_t(rank) : -int64_t(rank);
      book.add(1, next++, 0, Common::Side::BUY, 1000 + delta, 1);
      book.add(1, next++, 0, Common::Side::SELL, 5000 - delta, 1);
    }
    const auto ns = elapsed(start);
    if (round >= 30)
      samples.push_back(ns / (2 * depth));
    require(book.liveOrders().size() == 2 * depth);
    for (auto id = first; id < next; ++id)
      book.cancel(1, id, 0);
    require(book.liveOrders().empty());
  }
  require(accepted == (rounds + 30) * depth * 2 && accepted == updates);
  result(mode.c_str(), samples, accepted);
}

std::vector<U> snapshot(uint64_t cycle, size_t count = 64) {
  std::vector<E> orders;
  for (uint64_t id = 1; id <= count; ++id)
    orders.push_back({T::ADD, id, Common::TickerId(id % 8), Common::Side::BUY, 100, 2, id});
  const auto hash = Common::bookHash(orders);
  std::vector<U> frames;
  auto add = [&](E e) { frames.push_back({frames.size(), e, cycle, 42, hash}); };
  add({T::SNAPSHOT_START, 42});
  for (Common::TickerId i = 0; i < 8; ++i)
    add({T::CLEAR, Common::OrderId_INVALID, i});
  for (const auto& order : orders)
    add(order);
  add({T::SNAPSHOT_END, 42});
  return frames;
}

void recovery(const std::string& mode) {
  auto frames = snapshot(1, mode == "recovery_snapshot_1024" ? 1024 : 64);
  Trading::MarketRecovery r;
  std::vector<uint64_t> samples;
  uint64_t checksum = 0;
  for (size_t round = 0; round < 1100; ++round) {
    r.restart();
    for (auto& f : frames)
      f.snapshot_cycle_ = round + 1;
    const auto start = Clock::now();
    if (mode == "recovery_missing_frame") {
      for (size_t i = 0; i < frames.size(); ++i)
        if (i != 10)
          r.ingest(true, frames[i]);
      for (size_t i = 0; i < 8; ++i)
        require(!r.build());
      r.ingest(true, frames[10]);
    } else {
      for (const auto& frame : frames)
        r.ingest(true, frame);
      if (mode == "recovery_duplicates")
        for (size_t i = 0; i < 64; ++i)
          r.ingest(true, frames[10 + i]);
      if (mode == "recovery_incremental") {
        r.ingest(false, {43, {T::MODIFY, 1, 1, Common::Side::BUY, 100, 1, 1}});
        r.ingest(false, {44, {T::TRADE, Common::OrderId_INVALID, 1, Common::Side::BUY, 100, 1}});
      }
    }
    auto batch = r.build();
    require(bool(batch));
    r.commit(*batch);
    const auto ns = elapsed(start);
    require(batch->watermark == (mode == "recovery_incremental" ? 44 : 42));
    if (round >= 100) {
      samples.push_back(ns);
      checksum += batch->events.size();
    }
  }
  result(mode.c_str(), samples, checksum);
}

int main(int argc, char** argv) {
  try {
    require(argc == 2);
    const std::string mode = argv[1];
    if (mode.starts_with("book_"))
      book(mode);
    else
      recovery(mode);
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
