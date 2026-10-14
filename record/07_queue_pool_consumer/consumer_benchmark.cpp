// 仅用于 Linux/GCC 的消费微基准。私有可见性只在测试翻译单元打开。
// 标准头在宏之前加载，避免影响标准库类的访问控制。
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
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
#include <numeric>
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
#include "exchange/order_server/order_server.h"
#include "exchange/market_data/snapshot_synthesizer.h"
#undef private

using namespace Common;
using namespace Exchange;
using Clock = std::chrono::steady_clock;

void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

void drain_diagnostics(Logger& logger) {
  // 等待只发生在计时区外；两版相同普通日志文件、容量和后台刷新策略。
  while (logger.queue_.size())
    std::this_thread::yield();
  require(logger.dropped_records() == 0, "diagnostic log dropped; round invalid");
}

struct Measurement {
  static constexpr size_t Warmup = 64;
  static constexpr uint64_t TargetNanos = 200000000;
  std::vector<uint64_t> samples;
  uint64_t measured_nanos = 0, measured_operations = 0;

  bool more(size_t round) const {
    return round < Warmup || measured_nanos < TargetNanos;
  }

  void record(size_t round, const Clock::time_point begin, size_t operations) {
    const auto elapsed = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count());
    if (round >= Warmup) {
      measured_nanos += elapsed;
      measured_operations += operations;
      samples.push_back(elapsed);
    }
  }

  void output(const std::string& mode, size_t batch, uint64_t accepted, uint64_t checksum) {
    require(!samples.empty(), "no measured samples");
    std::sort(samples.begin(), samples.end());
    std::cout << "{\"mode\":\"" << mode
              << "\",\"p50_ns\":" << double(samples[samples.size() / 2]) / batch
              << ",\"p99_ns\":" << double(samples[samples.size() * 99 / 100]) / batch
              << ",\"samples\":" << samples.size() << ",\"measured_elapsed_ns\":" << measured_nanos
              << ",\"measured_operations\":" << measured_operations << ",\"batch_size\":" << batch
              << ",\"accepted\":" << accepted << ",\"checksum\":" << checksum
              << ",\"dropped_logs\":0}\n";
  }
};

void snapshot_benchmark() {
  MDPMarketUpdateLFQueue updates(4);
  auto snapshot = std::make_unique<SnapshotSynthesizer>(&updates, "lo", "233.252.16.1", 29100);
  drain_diagnostics(snapshot->logger_);
  const auto free_slots = snapshot->order_pool_.available();
  constexpr size_t Cycles = 256, Events = Cycles * 3;
  Measurement measurement;
  uint64_t sequence = 0, checksum = 0;
  for (size_t round = 0; measurement.more(round); ++round) {
    const auto before_checksum = checksum;
    const auto begin = Clock::now();
    for (size_t i = 0; i < Cycles; ++i) {
      MDPMarketUpdate event{++sequence, {MarketUpdateType::ADD, 17, 2, Side::BUY, 100, 5, 9}};
      snapshot->addToSnapshot(&event);
      event.seq_num_ = ++sequence;
      event.me_market_update_.type_ = MarketUpdateType::MODIFY;
      event.me_market_update_.price_ = 101;
      event.me_market_update_.qty_ = 3;
      snapshot->addToSnapshot(&event);
      const auto* object = snapshot->ticker_orders_[2][17];
      checksum += object->qty_ + object->price_ + object->priority_;
      event.seq_num_ = ++sequence;
      event.me_market_update_.type_ = MarketUpdateType::CANCEL;
      snapshot->addToSnapshot(&event);
    }
    measurement.record(round, begin, Events);
    require(checksum - before_checksum == Cycles * 113 && snapshot->last_inc_seq_num_ == sequence &&
                !snapshot->ticker_orders_[2][17] && snapshot->order_pool_.available() == free_slots,
            "snapshot state, quantity, sequence or pool differs");
    drain_diagnostics(snapshot->logger_);
  }
  measurement.output("snapshot", Events, sequence, checksum);
}

void fifo_benchmark(bool singleton) {
  Logger logger("consumer_fifo_benchmark.log");
  ClientRequestLFQueue requests(64);
  FIFOSequencer sequencer(&requests, &logger);
  const size_t batch = singleton ? 1 : 64;
  std::vector<size_t> expected(batch);
  std::iota(expected.begin(), expected.end(), 0);
  const auto timestamp = [](size_t i) { return Nanos((i * 7) % 11); };
  std::stable_sort(expected.begin(), expected.end(),
                   [&](size_t left, size_t right) { return timestamp(left) < timestamp(right); });
  Measurement measurement;
  uint64_t next = 1, accepted = 0, checksum = 0;
  for (size_t round = 0; measurement.more(round); ++round) {
    drain_diagnostics(logger);
    const auto first = next;
    for (size_t i = 0; i < batch; ++i) {
      require(sequencer.addClientRequest(timestamp(i),
                                         {ClientRequestType::NEW, 1, 0, next++, Side::BUY, 100, 1}),
              "FIFO setup refused request");
    }
    const auto begin = Clock::now();
    sequencer.sequenceAndPublish();
    measurement.record(round, begin, batch);
    require(sequencer.pending() == 0, "FIFO did not publish complete batch");
    for (auto index : expected) {
      MEClientRequest actual;
      require(requests.try_pop(actual) && actual.order_id_ == first + index &&
                  actual.client_id_ == 1 && actual.ticker_id_ == 0 && actual.side_ == Side::BUY &&
                  actual.price_ == 100 && actual.qty_ == 1,
              "FIFO timestamp stability or business fields differ");
      checksum += actual.order_id_;
      ++accepted;
    }
    require(!requests.peek(), "FIFO left an unexpected request");
  }
  drain_diagnostics(logger);
  measurement.output(singleton ? "fifo_singleton" : "fifo_batch", batch, accepted, checksum);
}

void response_benchmark() {
  constexpr size_t Batch = 64;
  ClientRequestLFQueue requests(4);
  ClientResponseLFQueue responses(128);
  Logger logger("consumer_connection_benchmark.log");
  TCPSocket connection(logger, Common::Wire::ResponseSize * Batch);
  OrderServer server(&requests, &responses, "lo", 0);
  constexpr ClientId Client = 7;
  constexpr uint64_t Epoch = 61;
  server.cid_tcp_socket_[Client] = &connection;
  server.cid_session_epoch_[Client] = Epoch;
  server.accepting_.store(false);
  Measurement measurement;
  uint64_t accepted = 0, checksum = 0;
  for (size_t round = 0; measurement.more(round); ++round) {
    drain_diagnostics(server.logger_);
    connection.next_send_valid_index_ = 0;
    connection.send_begin_ = 0;
    const auto first_sequence = server.cid_next_outgoing_seq_num_[Client];
    for (size_t i = 0; i < Batch; ++i) {
      const bool fill = i % 2;
      MEClientResponse response{fill ? ClientResponseType::FILLED : ClientResponseType::ACCEPTED,
                                Client,
                                0,
                                i + 1,
                                i + 101,
                                Side::BUY,
                                100,
                                fill ? Qty(2) : Qty(0),
                                fill ? Qty(3) : Qty(5)};
      response.response_id_ = accepted + i + 1;
      response.position_ = fill ? 2 : 0;
      require(responses.try_push(response), "response setup full");
    }
    const auto begin = Clock::now();
    // 测真实回报队列排空、编码和入TCP用户态缓冲；不测实际send系统调用。
    server.run();
    measurement.record(round, begin, Batch);
    require(!responses.peek() && connection.pending_bytes() == Batch * Common::Wire::ResponseSize &&
                server.cid_next_outgoing_seq_num_[Client] == first_sequence + Batch,
            "response consumption or sequence differs");
    for (size_t i = 0; i < Batch; ++i) {
      OMClientResponse actual;
      require(
          Common::Wire::decode(reinterpret_cast<const uint8_t*>(connection.outbound_data_.data()) +
                                   i * Common::Wire::ResponseSize,
                               actual),
          "response decode failed");
      const auto& response = actual.me_client_response_;
      const bool fill = i % 2;
      require(actual.seq_num_ == first_sequence + i && actual.session_epoch_ == Epoch &&
                  response.type_ ==
                      (fill ? ClientResponseType::FILLED : ClientResponseType::ACCEPTED) &&
                  response.client_id_ == Client && response.client_order_id_ == i + 1 &&
                  response.market_order_id_ == i + 101 && response.side_ == Side::BUY &&
                  response.price_ == 100 && response.exec_qty_ == (fill ? 2 : 0) &&
                  response.leaves_qty_ == (fill ? 3 : 5) &&
                  response.response_id_ == accepted + i + 1 && response.position_ == (fill ? 2 : 0),
              "response FIFO, epoch or business fields differ");
      checksum += response.client_order_id_ + response.exec_qty_ + response.leaves_qty_;
    }
    accepted += Batch;
    require(!requests.peek() && server.accepted_frames_ == 0,
            "response consumption changed request admission");
  }
  drain_diagnostics(server.logger_);
  require(logger.dropped_records() == 0, "connection logger dropped");
  measurement.output("response", Batch, accepted, checksum);
}

int main(int argc, char** argv) {
  try {
    require(argc == 2, "provide snapshot, response, fifo_singleton or fifo_batch");
    const std::string mode = argv[1];
    if (mode == "snapshot")
      snapshot_benchmark();
    else if (mode == "response")
      response_benchmark();
    else if (mode == "fifo_singleton" || mode == "fifo_batch")
      fifo_benchmark(mode == "fifo_singleton");
    else
      throw std::runtime_error("unknown consumer benchmark mode");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
