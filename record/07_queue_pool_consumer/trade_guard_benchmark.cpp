#include "trade_guard_probe.h"

using namespace TradeGuardProbe;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
  try {
    require(argc == 1 || argc == 2, "provide optional measured batch count");
    const auto fixed = argc == 2 ? std::stoull(argv[1]) : uint64_t(0);
    require(fixed <= 1000000, "excessive batch count");
    constexpr uint64_t Warmup = 32, Cycles = 32, Batch = Cycles * 3;
    constexpr uint64_t CalibrationNanos = 200000000;
    uint64_t elapsed = 0, measured_batches = 0, all_cycles = 0, checksum = 0;
    std::vector<uint64_t> samples;
    {
      Fixture fixture;  // 构造、首次恢复、启动日志和预置卖单均不计时。
      fixture.trust();
      fixture.seed();
      const auto generation = fixture.engine->marketGeneration().load();
      const auto order_free = fixture.book().order_pool_.available();
      const auto price_free = fixture.book().orders_at_price_pool_.available();
      for (uint64_t round = 0;
           round < Warmup || (fixed ? measured_batches < fixed : elapsed < CalibrationNanos);
           ++round) {
        drain(fixture.engine->logger_);
        for (uint64_t cycle = 0; cycle < Cycles; ++cycle)
          fixture.enqueueCycle();  // 队列装填不计时，两版每批固定96条相同合法事件。
        const auto begin = Clock::now();
        fixture.engine->run();  // 真实消费队列 → 本地簿 → 特征 → 策略回调。
        const auto duration = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
        if (round >= Warmup) {
          elapsed += duration;
          ++measured_batches;
          samples.push_back(duration);
        }
        all_cycles += Cycles;
        fixture.validateEnd(order_free, price_free, generation, all_cycles);
        drain(fixture.engine->logger_);
      }
      checksum = fixture.observer->checksum;
      require(fixture.engine->logger_.dropped_records() == 0, "benchmark dropped logs");
    }
    validateFooter();  // 析构后的普通日志文件也必须声明零丢弃。
    require(!samples.empty() && elapsed >= CalibrationNanos, "measurement shorter than 200ms");
    std::sort(samples.begin(), samples.end());
    std::cout << "{\"mode\":\"trade_run\",\"mean_ns_per_event\":"
              << double(elapsed) / double(measured_batches * Batch)
              << ",\"p50_batch_mean_ns_per_event\":"
              << double(samples[samples.size() / 2]) / Batch
              << ",\"p99_batch_mean_ns_per_event\":"
              << double(samples[samples.size() * 99 / 100]) / Batch
              << ",\"measured_elapsed_ns\":" << elapsed
              << ",\"measured_batches\":" << measured_batches
              << ",\"measured_events\":" << measured_batches * Batch
              << ",\"warmup_batches\":" << Warmup << ",\"batch_size\":" << Batch
              << ",\"callbacks_including_warmup\":" << all_cycles * 3
              << ",\"checksum_including_warmup\":" << checksum
              << ",\"dropped_logs\":0,\"final_book_orders\":1}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
