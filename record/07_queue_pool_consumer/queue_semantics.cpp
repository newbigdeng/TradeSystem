#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "common/logging.h"

namespace {
void require(bool ok, const char* reason) {
  if (!ok)
    throw std::runtime_error(reason);
}

void interfaces() {
  Common::LFQueue<uint64_t> q(4);
  uint64_t value = 0;
  for (uint64_t i = 0; i < 10000; ++i) {
    require(q.try_push(i), "single admission failed");
    const auto* first = q.peek();
    require(first && *first == i && q.peek() == first, "repeated peek changed borrowed head");
    if (i % 2)
      q.pop();
    else
      require(q.try_pop(value) && value == i, "peek/try_pop mismatch");
    require(!q.peek() && q.high_watermark() == 1, "queue reported fake occupancy");
  }
  require(q.try_push_batch({0, 1, 2, 3}), "full-capacity batch failed");
  require(q.try_push_batch({}) && q.size() == 4 && q.full_count() == 0,
          "empty batch changed full queue");
  require(!q.try_push(4) && !q.try_push_batch({4}), "full queue accepted payload");
  require(q.full_count() == 2 && q.high_watermark() == 4, "full counters inaccurate");
  for (uint64_t i = 0; i < 4; ++i) {
    require(q.try_pop(value) && value == i, "empty/failed batch modified values");
  }
  require(q.try_push_batch({}) && !q.peek() && q.full_count() == 2,
          "empty batch changed empty queue");
}

void loggerProducers() {
  // 故意让小队列拥堵：允许诊断日志整条丢弃，必须正确计数且不能重复写。
  constexpr uint64_t producers = 4, records = 5000;
  const auto path = std::filesystem::path("queue_semantics_logger.log");
  uint64_t dropped = 0;
  {
    Common::Logger logger(path.string(), 128);
    std::vector<std::thread> threads;
    for (uint64_t producer = 0; producer < producers; ++producer) {
      threads.emplace_back([&logger, producer] {
        for (uint64_t i = 0; i < records; ++i)
          logger.log("T:%:%\n", producer, i);
      });
    }
    for (auto& thread : threads)
      thread.join();
    dropped = logger.dropped_records();
  }
  std::ifstream input(path);
  require(input.is_open(), "logger output absent");
  std::vector<bool> seen(producers * records);
  std::string line;
  uint64_t received = 0, footer = UINT64_MAX;
  while (std::getline(input, line)) {
    if (line.starts_with("[LOGGER] dropped_records=")) {
      footer = std::stoull(line.substr(25));
      continue;
    }
    std::replace(line.begin(), line.end(), ':', ' ');
    std::istringstream fields(line);
    char prefix = 0;
    uint64_t producer = 0, number = 0;
    require(bool(fields >> prefix >> producer >> number) && prefix == 'T' && producer < producers &&
                number < records,
            "logger malformed record");
    const auto index = producer * records + number;
    require(!seen[index], "logger duplicate record");
    seen[index] = true;
    ++received;
  }
  require(footer == dropped && received + dropped == producers * records,
          "logger serialized producers lost uncounted records");
  std::cout << "logger_received=" << received << " dropped=" << dropped << '\n';
}
}

int main() {
  try {
    interfaces();
    loggerProducers();
    std::cout
        << "PASS repeated peek, mixed consumer operations, empty/full batch, real high watermark, serialized logger producers\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
