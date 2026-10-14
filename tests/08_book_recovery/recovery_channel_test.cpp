#include "common/mcast_socket.h"
#include "common/market_protocol.h"
#include "trading/market_data/market_recovery.h"

// 只开放消费器的测试入口，依赖头已预先加载；无需额外生产接口或启动后台接收线程。
#define private public
#include "trading/market_data/market_data_consumer.h"
#undef private

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool result, const char* message) {
  if (!result)
    throw std::runtime_error(message);
}

void load(Common::McastSocket& socket, const Exchange::MDPMarketUpdate& update) {
  const auto bytes = Common::Wire::encode(update);
  std::memcpy(socket.inbound_data_.data(), bytes.data(), bytes.size());
  socket.next_rcv_valid_index_ = bytes.size();
  socket.receive_fault_ = false;
}

void benchmark(Trading::MarketDataConsumer& consumer, Exchange::MEMarketUpdateLFQueue& output) {
  using Type = Exchange::MarketUpdateType;
  Exchange::MDPMarketUpdate incremental;
  incremental.seq_num_ = 1;
  incremental.me_market_update_ = {Type::CLEAR, Common::OrderId_INVALID, 0};
  load(consumer.incremental_, incremental);
  uint64_t seq = 1, delivered = 0, type_sum = 0, ticker_sum = 0;
  const auto updates = [&](size_t count) {
    for (size_t i = 0; i < count; ++i) {
      // 固定帧只改序号；旧/新版本承担相同的编码和消费成本。
      auto* cursor = reinterpret_cast<uint8_t*>(consumer.incremental_.inbound_data_.data() + 4);
      Common::Wire::put(cursor, seq++);
      consumer.incremental_.next_rcv_valid_index_ = Common::Wire::MarketSize;
      consumer.receive(&consumer.incremental_);
      Exchange::MEMarketUpdate actual;
      delivered += output.try_pop(actual);
      type_sum += static_cast<uint8_t>(actual.type_);
      ticker_sum += actual.ticker_id_;
    }
  };
  updates(100);
  std::vector<double> samples;
  samples.reserve(100);
  for (size_t sample = 0; sample < 100; ++sample) {
    const auto begin = std::chrono::steady_clock::now();
    updates(100000);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - begin)
                             .count();
    samples.push_back(double(elapsed) / 100000);
  }
  check(delivered == 10000100 && type_sum == delivered * static_cast<uint8_t>(Type::CLEAR) &&
            ticker_sum == 0 && consumer.recovery_.expected() == seq &&
            !consumer.recovery_.recovering() && !output.peek(),
        "receive benchmark lost or altered an update");
  std::sort(samples.begin(), samples.end());
  std::cout << "{\"mode\":\"receive\",\"updates\":10000000,\"p50_ns\":" << samples[50]
            << ",\"p99_ns\":" << samples[99] << ",\"delivered\":" << delivered << "}\n";
}
}

int main(int argc, char** argv) {
  try {
    using Type = Exchange::MarketUpdateType;
    Exchange::MEMarketUpdateLFQueue output(64);
    std::atomic<uint64_t> generation{1};
    Trading::MarketDataConsumer consumer(1, &output, "lo", "233.252.14.61", 33070, "233.252.14.62",
                                         33071, &generation);
    const auto live = [&] { consumer.recovery_.commit({{}, 0}); };
    live();
    if (argc == 2 && std::string(argv[1]) == "benchmark") {
      benchmark(consumer, output);
      return 0;
    }
    const auto old_generation = generation.load();
    Exchange::MDPMarketUpdate snapshot;
    snapshot.seq_num_ = 0;
    snapshot.snapshot_cycle_ = 1;
    snapshot.me_market_update_.type_ = Type::SNAPSHOT_START;

    // baseline 的独立 channel 模式用于分别复现两类缺陷。
    const bool only_channel = argc == 2 && std::string(argv[1]) == "channel";
    if (!only_channel) {
      // 实时增量健康时，周期快照及坏快照不应打断已恢复状态。
      load(consumer.snapshot_, snapshot);
      consumer.receive(&consumer.snapshot_);
      check(
          !consumer.recovery_.recovering() && generation.load() == old_generation && !output.peek(),
          "healthy periodic snapshot interrupted live market");
      load(consumer.snapshot_, snapshot);
      consumer.snapshot_.inbound_data_[0] = '?';
      consumer.receive(&consumer.snapshot_);
      check(
          !consumer.recovery_.recovering() && generation.load() == old_generation && !output.peek(),
          "bad periodic snapshot poisoned healthy incrementals");
      consumer.snapshot_.next_rcv_valid_index_ = 1;
      consumer.receive(&consumer.snapshot_);
      check(!consumer.recovery_.recovering() && generation.load() == old_generation,
            "short periodic snapshot poisoned healthy incrementals");
      load(consumer.snapshot_, snapshot);
      consumer.snapshot_.receive_fault_ = true;
      consumer.receive(&consumer.snapshot_);
      check(!consumer.recovery_.recovering() && generation.load() == old_generation,
            "truncated periodic snapshot poisoned healthy incrementals");
    }

    // 快照控制帧混入增量通道必须失效，不能流入订单簿触发非法品种访问。
    for (const auto type : {Type::SNAPSHOT_START, Type::SNAPSHOT_END}) {
      live();
      auto misplaced = snapshot;
      misplaced.seq_num_ = 1;
      misplaced.me_market_update_.type_ = type;
      const auto before = generation.load();
      load(consumer.incremental_, misplaced);
      consumer.receive(&consumer.incremental_);
      check(consumer.recovery_.recovering() && generation.load() > before && !output.peek(),
            "incremental channel forwarded snapshot control frame");
    }

    // 恢复中仍需验证快照报文；坏帧不能假装恢复成功。
    consumer.recovery_.restart();
    const auto before_bad = generation.load();
    load(consumer.snapshot_, snapshot);
    consumer.snapshot_.inbound_data_[0] = '?';
    consumer.receive(&consumer.snapshot_);
    check(consumer.recovery_.recovering() && generation.load() > before_bad && !output.peek(),
          "bad snapshot was accepted during recovery");

    // 合法增量继续准入，序号只在队列接收成功后推进。
    live();
    Exchange::MDPMarketUpdate incremental;
    incremental.seq_num_ = 1;
    incremental.me_market_update_ = {Type::CLEAR, Common::OrderId_INVALID, 0};
    load(consumer.incremental_, incremental);
    consumer.receive(&consumer.incremental_);
    Exchange::MEMarketUpdate actual;
    check(output.try_pop(actual) && actual.type_ == Type::CLEAR && actual.ticker_id_ == 0 &&
              consumer.recovery_.expected() == 2 && !consumer.recovery_.recovering(),
          "legitimate incremental was broken by channel guard");
    std::cout
        << "PASS periodic snapshots, malformed snapshots, channel separation, live sequence\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
