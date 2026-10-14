#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <pthread.h>
#include <sched.h>
#include "common/lf_queue.h"

namespace {
using Clock = std::chrono::steady_clock;

// 输出整窗耗时 / 消息数（full 为失败调用数），不能当作单消息 P50/P99。
// 固定输入、交替运行多轮后再统计；不要删除慢轮或把吞吐当作个体延迟。
struct Result {
  uint64_t messages = 0;
  // cross 的 calls 不含消费者空轮询；仅 full 模式使用它作为计时分母。
  uint64_t calls = 0;
  uint64_t sum = 0;
  uint64_t elapsed_ns = 0;
  uint64_t full = 0;
  size_t high = 0;
};

void require(bool value, const char* reason) {
  if (!value)
    throw std::runtime_error(reason);
}

void pin(int cpu) {
  require(cpu >= 0 && cpu < CPU_SETSIZE, "invalid CPU number");
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  require(pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0, "affinity failed");
}

uint64_t elapsed(Clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
}

Result pair(uint64_t count) {
  Common::LFQueue<uint64_t> q(1024);
  uint64_t value = 0, sum = 0;
  for (uint64_t i = 0; i < 10000; ++i)
    require(q.try_push(i) && q.try_pop(value) && value == i, "pair warmup mismatch");
  const auto start = Clock::now();
  for (uint64_t i = 0; i < count; ++i) {
    require(q.try_push(i) && q.try_pop(value) && value == i, "pair FIFO mismatch");
    sum += value;
  }
  const auto duration = elapsed(start);
  require(sum == count * (count - 1) / 2 && q.size() == 0, "pair missing messages");
  require(q.full_count() == 0 && q.high_watermark() == 1, "pair diagnostics changed");
  return {count, count * 2, sum, duration, q.full_count(), q.high_watermark()};
}

Result batch(uint64_t count) {
  constexpr size_t batch_size = 16;
  require(count % batch_size == 0, "batch count must be divisible by 16");
  Common::LFQueue<uint64_t> q(1024);
  std::vector<uint64_t> values(batch_size);
  uint64_t sum = 0;
  for (uint64_t warm = 0; warm < 1000; ++warm) {
    require(q.try_push_batch(values), "batch warmup admission failed");
    for (size_t i = 0; i < batch_size; ++i) {
      const auto* value = q.peek();
      require(value && *value == 0, "batch warmup mismatch");
      q.pop();
    }
  }
  const auto start = Clock::now();
  for (uint64_t base = 0; base < count; base += batch_size) {
    for (size_t i = 0; i < batch_size; ++i)
      values[i] = base + i;
    require(q.try_push_batch(values), "batch admission failed");
    for (size_t i = 0; i < batch_size; ++i) {
      const auto* value = q.peek();
      require(value && *value == base + i, "batch FIFO mismatch");
      sum += *value;
      q.pop();
    }
  }
  const auto duration = elapsed(start);
  require(sum == count * (count - 1) / 2 && q.size() == 0, "batch missing messages");
  require(q.full_count() == 0 && q.high_watermark() == batch_size, "batch diagnostics changed");
  return {count, count / batch_size + count * 2, sum, duration, q.full_count(), q.high_watermark()};
}

Result cross(uint64_t count, int producer_cpu, int consumer_cpu) {
  struct Packet {
    uint64_t id, inverse;
  };

  Common::LFQueue<Packet> q(1024);
  std::atomic<bool> ready{false}, go{false};
  uint64_t sum = 0, retries = 0;
  bool correct = true;
  std::thread consumer([&] {
    pin(consumer_cpu);
    ready.store(true, std::memory_order_release);
    while (!go.load(std::memory_order_acquire)) {
    }
    for (uint64_t i = 0; i < count; ++i) {
      const Packet* value = nullptr;
      while (!(value = q.peek())) {
      }
      correct &= value->id == i && value->inverse == ~i;
      sum += value->id;
      q.pop();
    }
  });
  pin(producer_cpu);
  while (!ready.load(std::memory_order_acquire)) {
  }
  const auto start = Clock::now();
  go.store(true, std::memory_order_release);
  for (uint64_t i = 0; i < count; ++i) {
    const Packet value{i, ~i};
    while (!q.try_push(value))
      ++retries;
  }
  consumer.join();
  const auto duration = elapsed(start);
  require(correct && sum == count * (count - 1) / 2 && q.size() == 0, "cross FIFO/torn payload");
  require(q.full_count() == retries && q.high_watermark() <= 1024 && q.high_watermark() > 0,
          "cross diagnostics changed");
  return {count, count * 3 + retries, sum, duration, q.full_count(), q.high_watermark()};
}

Result full(uint64_t count) {
  Common::LFQueue<uint64_t> q(8);
  for (uint64_t i = 0; i < 8; ++i)
    require(q.try_push(i), "full prefill failed");
  const std::vector<uint64_t> one{42};
  const auto start = Clock::now();
  for (uint64_t i = 0; i < count; ++i)
    require(!q.try_push(42) && !q.try_push_batch(one), "full admission overwrote payload");
  const auto duration = elapsed(start);
  require(q.full_count() == count * 2 && q.high_watermark() == 8 && q.size() == 8,
          "full counters changed");
  uint64_t value = 0, sum = 0;
  for (uint64_t i = 0; i < 8; ++i) {
    require(q.try_pop(value) && value == i, "full failed admission modified payload");
    sum += value;
  }
  require(q.size() == 0, "full drain failed");
  return {0, count * 2, sum, duration, q.full_count(), q.high_watermark()};
}
}

int main(int argc, char** argv) {
  try {
    require(argc >= 3 && argc <= 5, "usage: MODE COUNT [PRODUCER_CPU CONSUMER_CPU]");
    const std::string mode = argv[1];
    const uint64_t count = std::stoull(argv[2]);
    require(count > 0 && count <= 1000000000, "count out of range");
    const int producer_cpu = argc > 3 ? std::stoi(argv[3]) : 3;
    const int consumer_cpu = argc > 4 ? std::stoi(argv[4]) : 2;
    pin(producer_cpu);
    Result result;
    if (mode == "pair")
      result = pair(count);
    else if (mode == "batch")
      result = batch(count);
    else if (mode == "cross") {
      cross(20000, producer_cpu, consumer_cpu);
      result = cross(count, producer_cpu, consumer_cpu);
    } else if (mode == "full")
      result = full(count);
    else
      throw std::runtime_error("unknown benchmark mode");
    const double denominator = mode == "full" ? result.calls : result.messages;
    std::cout << "{\"mode\":\"" << mode << "\",\"messages\":" << result.messages
              << ",\"calls\":" << result.calls << ",\"sum\":" << result.sum
              << ",\"elapsed_ns\":" << result.elapsed_ns
              << ",\"ns_per_unit\":" << double(result.elapsed_ns) / denominator
              << ",\"full_count\":" << result.full << ",\"high_watermark\":" << result.high
              << "}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
