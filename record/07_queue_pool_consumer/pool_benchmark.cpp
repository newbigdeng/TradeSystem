#include <algorithm>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>
#include "common/mem_pool.h"
#include "exchange/matcher/me_order.h"
#include "exchange/market_data/market_update.h"
#include "trading/strategy/market_order.h"

using Clock = std::chrono::steady_clock;

void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

template<class T> struct Payload;

template<> struct Payload<Exchange::MEOrder> {
  static auto* allocate(Common::MemPool<Exchange::MEOrder>& pool, uint64_t id) {
    return pool.allocate(0, 1, id, id, Common::Side::BUY, 10000 + int64_t(id % 256),
                         Common::Qty(id % 128 + 1), id, nullptr, nullptr);
  }
  static bool valid(const Exchange::MEOrder* value, uint64_t id) {
    return value->ticker_id_ == 0 && value->client_id_ == 1 && value->client_order_id_ == id &&
           value->market_order_id_ == id && value->side_ == Common::Side::BUY &&
           value->price_ == 10000 + int64_t(id % 256) && value->qty_ == id % 128 + 1 &&
           value->priority_ == id && !value->prev_order_ && !value->next_order_;
  }
};

template<> struct Payload<Trading::MarketOrder> {
  static auto* allocate(Common::MemPool<Trading::MarketOrder>& pool, uint64_t id) {
    return pool.allocate(id, Common::Side::BUY, 10000 + int64_t(id % 256),
                         Common::Qty(id % 128 + 1), id, nullptr, nullptr);
  }
  static bool valid(const Trading::MarketOrder* value, uint64_t id) {
    return value->order_id_ == id && value->side_ == Common::Side::BUY &&
           value->price_ == 10000 + int64_t(id % 256) && value->qty_ == id % 128 + 1 &&
           value->priority_ == id && !value->prev_order_ && !value->next_order_;
  }
};

template<> struct Payload<Exchange::MEOrdersAtPrice> {
  static auto* allocate(Common::MemPool<Exchange::MEOrdersAtPrice>& pool, uint64_t id) {
    return pool.allocate(Common::Side::BUY, 10000 + int64_t(id), nullptr, nullptr, nullptr);
  }
  static bool valid(const Exchange::MEOrdersAtPrice* value, uint64_t id) {
    return value->side_ == Common::Side::BUY && value->price_ == 10000 + int64_t(id) &&
           !value->first_me_order_ && !value->prev_entry_ && !value->next_entry_;
  }
};

template<> struct Payload<Trading::MarketOrdersAtPrice> {
  static auto* allocate(Common::MemPool<Trading::MarketOrdersAtPrice>& pool, uint64_t id) {
    return pool.allocate(Common::Side::BUY, 10000 + int64_t(id), nullptr, nullptr, nullptr);
  }
  static bool valid(const Trading::MarketOrdersAtPrice* value, uint64_t id) {
    return value->side_ == Common::Side::BUY && value->price_ == 10000 + int64_t(id) &&
           !value->first_mkt_order_ && !value->prev_entry_ && !value->next_entry_;
  }
};

template<> struct Payload<Exchange::MEMarketUpdate> {
  static auto* allocate(Common::MemPool<Exchange::MEMarketUpdate>& pool, uint64_t id) {
    const Exchange::MEMarketUpdate event{
        Exchange::MarketUpdateType::ADD, id, 0, Common::Side::BUY,
        10000 + int64_t(id % 256), Common::Qty(id % 128 + 1), id};
    return pool.allocate(event);
  }
  static bool valid(const Exchange::MEMarketUpdate* value, uint64_t id) {
    return value->type_ == Exchange::MarketUpdateType::ADD && value->order_id_ == id &&
           value->ticker_id_ == 0 && value->side_ == Common::Side::BUY &&
           value->price_ == 10000 + int64_t(id % 256) && value->qty_ == id % 128 + 1 &&
           value->priority_ == id;
  }
};

// 编译器屏障只阻止把整个借出/归还循环折叠；不生成 CPU fence 或系统调用。
template<class T> inline void observe(T* value) {
  asm volatile("" : : "r"(value) : "memory");
}

template<class T>
void benchmark(const std::string& name, const std::string& workload, size_t capacity,
               size_t cycles, const std::string& raw_path) {
  require(capacity > 0 && capacity <= Common::ME_MAX_ORDER_IDS && cycles > 0,
          "benchmark argument outside fixed limits");
  require(workload == "reuse" || workload == "random", "unknown workload");
  require(workload != "reuse" || capacity == 1, "reuse workload requires one slot");
  Common::MemPool<T> pool(capacity);
  std::vector<T*> objects(capacity);
  std::vector<size_t> release_order(capacity);
  std::iota(release_order.begin(), release_order.end(), 0);
  std::mt19937 random(19850407);
  std::shuffle(release_order.begin(), release_order.end(), random);
  std::vector<uint64_t> samples;
  samples.reserve(cycles);
  const size_t pairs_per_cycle = workload == "reuse" ? 8192 : capacity;

  const auto cycle = [&]() -> uint64_t {
    uint64_t sum = 0;
    if (workload == "reuse") {
      for (uint64_t id = 1; id <= pairs_per_cycle; ++id) {
        auto* value = Payload<T>::allocate(pool, id);
        require(value != nullptr, "single-slot allocation unexpectedly exhausted");
        observe(value);
        require(Payload<T>::valid(value, id), "single-slot payload changed");
        sum += id;
        pool.deallocate(value);
      }
    } else {
      for (size_t i = 0; i < capacity; ++i) {
        objects[i] = Payload<T>::allocate(pool, i + 1);
        require(objects[i] != nullptr, "burst allocation unexpectedly exhausted");
      }
      observe(objects.data());
      require(pool.available() == 0, "full burst left unallocated slots");
      for (const auto index : release_order) {
        require(Payload<T>::valid(objects[index], index + 1), "burst payload changed");
        sum += index + 1;
        pool.deallocate(objects[index]);
      }
    }
    require(pool.available() == capacity, "cycle did not return every slot");
    return sum;
  };

  const uint64_t per_cycle = uint64_t(pairs_per_cycle) * (pairs_per_cycle + 1) / 2;
  for (size_t warm = 0; warm < 8; ++warm)
    require(cycle() == per_cycle, "warmup sum changed");
  uint64_t sum = 0;
  const auto begin = Clock::now();
  for (size_t i = 0; i < cycles; ++i) {
    const auto start = Clock::now();
    sum += cycle();
    samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start)
                          .count());
  }
  const uint64_t elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
  require(sum == per_cycle * cycles && pool.available() == capacity,
          "final count or sum differs");

  std::ofstream raw(raw_path);
  require(bool(raw), "could not open sample file");
  raw << "cycle,pairs,elapsed_ns\n";
  for (size_t i = 0; i < samples.size(); ++i)
    raw << i << ',' << pairs_per_cycle << ',' << samples[i] << '\n';
  raw.close();
  require(bool(raw), "sample file write failed");
  std::sort(samples.begin(), samples.end());
  std::cout << "{\"type\":\"" << name << "\",\"workload\":\"" << workload
            << "\",\"capacity\":" << capacity << ",\"cycles\":" << cycles
            << ",\"pairs\":" << uint64_t(cycles) * pairs_per_cycle
            << ",\"elapsed_ns\":" << elapsed
            << ",\"p50_pair_ns\":" << double(samples[samples.size() / 2]) / pairs_per_cycle
            << ",\"p99_pair_ns\":" << double(samples[samples.size() * 99 / 100]) / pairs_per_cycle
            << ",\"mean_pair_ns\":" << double(elapsed) / (cycles * pairs_per_cycle)
            << ",\"validated_sum\":" << sum << ",\"available\":" << pool.available()
            << ",\"sizeof\":" << sizeof(T) << ",\"align\":" << alignof(T) << "}\n";
}

int main(int argc, char** argv) {
  try {
    require(argc == 6, "usage: pool_benchmark TYPE WORKLOAD CAPACITY CYCLES RAW_CSV");
    const std::string type = argv[1], workload = argv[2];
    const size_t capacity = std::stoull(argv[3]), cycles = std::stoull(argv[4]);
    if (type == "me_order")
      benchmark<Exchange::MEOrder>(type, workload, capacity, cycles, argv[5]);
    else if (type == "market_order")
      benchmark<Trading::MarketOrder>(type, workload, capacity, cycles, argv[5]);
    else if (type == "me_level")
      benchmark<Exchange::MEOrdersAtPrice>(type, workload, capacity, cycles, argv[5]);
    else if (type == "market_level")
      benchmark<Trading::MarketOrdersAtPrice>(type, workload, capacity, cycles, argv[5]);
    else if (type == "market_update")
      benchmark<Exchange::MEMarketUpdate>(type, workload, capacity, cycles, argv[5]);
    else
      throw std::runtime_error("unknown payload type");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
