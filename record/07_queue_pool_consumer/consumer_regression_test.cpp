#include <chrono>
#include <functional>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include "common/critical_journal.h"
#include "common/order_protocol.h"
#include "common/tcp_server.h"
#include "common/mcast_socket.h"
#include "common/mem_pool.h"
#include "exchange/order_server/fifo_sequencer.h"
#include "exchange/matcher/me_order.h"
#include "exchange/market_data/market_update.h"

// 测试只检查内部状态；生产接口不为测试扩大可见性。
#define private public
#include "exchange/order_server/order_server.h"
#include "exchange/market_data/snapshot_synthesizer.h"
#undef private

using namespace Common;
using namespace Exchange;

void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

void logger_bytes_case() {
  const std::string payload(512, 'x');
  {
    Logger logger("consumer_logger_bytes.log");
    logger.log("%% literal:% enum:% ratio:% signed:% text:%\n", 42, "BUY", 1.25, -3,
               std::string("zero"));
    logger.log("unicode:%\n", std::string("中文注释"));
    logger.log("long:%\n", payload);
    require(logger.dropped_records() == 0, "logger byte test dropped record");
  }
  std::ifstream input("consumer_logger_bytes.log", std::ios::binary);
  std::ostringstream actual;
  actual << input.rdbuf();
  require(actual.str() ==
              "% literal:42 enum:BUY ratio:1.25 signed:-3 text:zero\n"
              "unicode:中文注释\nlong:" +
                  payload + "\n[LOGGER] dropped_records=0\n",
          "logger changed text, percent escape, UTF8 bytes or FIFO");
}

void response_state_case() {
  ClientRequestLFQueue requests(4);
  ClientResponseLFQueue responses(4);
  Logger logger("consumer_response_probe.log");
  TCPSocket connection(logger, Common::Wire::ResponseSize * 3);
  OrderServer server(&requests, &responses, "lo", 0);
  constexpr ClientId client = 7;
  constexpr uint64_t epoch = 61;
  server.cid_tcp_socket_[client] = &connection;
  server.cid_session_epoch_[client] = epoch;
  const MEClientResponse expected[]{
      {ClientResponseType::ACCEPTED, client, 0, 11, 101, Side::BUY, 100, 0, 5},
      {ClientResponseType::FILLED, client, 0, 11, 101, Side::BUY, 100, 2, 3},
      {ClientResponseType::CANCELED, client, 0, 11, 101, Side::BUY, 100, 0, 3}};
  for (size_t i = 0; i < 3; ++i) {
    auto response = expected[i];
    response.response_id_ = i + 1;
    response.position_ = i ? 2 : 0;
    require(responses.try_push(response), "response setup full");
  }

  // 不启动网络线程，只排空已有回报到用户态发送缓冲。
  server.run();
  require(!responses.peek() && server.cid_next_outgoing_seq_num_[client] == 4,
          "response slots or sequence changed after enqueue");
  require(connection.pending_bytes() == 3 * Common::Wire::ResponseSize,
          "response frame count differs");
  for (size_t i = 0; i < 3; ++i) {
    OMClientResponse actual;
    const auto* data = reinterpret_cast<const uint8_t*>(connection.outbound_data_.data()) +
                       i * Common::Wire::ResponseSize;
    require(Common::Wire::decode(data, actual), "response decode failed");
    const auto& response = actual.me_client_response_;
    require(actual.seq_num_ == i + 1 && actual.session_epoch_ == epoch,
            "wire FIFO or epoch changed");
    require(response.type_ == expected[i].type_ && response.client_id_ == client &&
                response.client_order_id_ == 11 && response.market_order_id_ == 101 &&
                response.side_ == Side::BUY && response.price_ == 100 &&
                response.exec_qty_ == expected[i].exec_qty_ &&
                response.leaves_qty_ == expected[i].leaves_qty_ && response.response_id_ == i + 1 &&
                response.position_ == (i ? 2 : 0),
            "response business fields changed");
  }
  require(!requests.peek() && server.accepted_frames_ == 0,
          "sending responses changed request admission");
}

void fifo_state_case() {
  Logger logger("consumer_fifo_probe.log");
  ClientRequestLFQueue requests(2);
  FIFOSequencer sequencer(&requests, &logger);
  auto add = [&](Nanos timestamp, OrderId id) {
    require(sequencer.addClientRequest(timestamp,
                                       {ClientRequestType::NEW, 1, 0, id, Side::BUY, 100, 1}),
            "FIFO setup refused request");
  };
  auto take = [&](OrderId id) {
    MEClientRequest actual;
    require(requests.try_pop(actual) && actual.order_id_ == id,
            "FIFO output order or quantity differs");
    require(actual.client_id_ == 1 && actual.ticker_id_ == 0 && actual.side_ == Side::BUY &&
                actual.price_ == 100 && actual.qty_ == 1,
            "FIFO changed request fields");
  };
  add(11, 1);
  sequencer.sequenceAndPublish();
  require(sequencer.pending() == 0, "singleton remained pending");
  take(1);

  // 时间戳乱序；相同时间戳保持进入暂存区的顺序。
  add(3, 2);
  add(1, 3);
  add(1, 4);
  add(2, 5);
  sequencer.sequenceAndPublish();
  require(sequencer.pending() == 2, "full queue lost unpublished requests");
  // 队列还满时重试，必须完整保留两个待发请求。
  sequencer.sequenceAndPublish();
  require(sequencer.pending() == 2, "zero-progress retry changed pending requests");
  take(3);
  take(4);
  sequencer.sequenceAndPublish();
  require(sequencer.pending() == 0, "retry did not finish FIFO publishing");
  take(5);
  take(2);
  require(!requests.peek() && logger.dropped_records() == 0,
          "FIFO test left output or dropped diagnostic records");
}

std::unique_ptr<SnapshotSynthesizer> make_snapshot(MDPMarketUpdateLFQueue& updates) {
  return std::make_unique<SnapshotSynthesizer>(&updates, "lo", "233.252.16.1", 29100);
}

void snapshot_state_case() {
  MDPMarketUpdateLFQueue updates(4);
  auto snapshot = make_snapshot(updates);
  const auto initial_free = snapshot->order_pool_.available();
  MDPMarketUpdate event{1, {MarketUpdateType::ADD, 17, 2, Side::BUY, 100, 5, 9}};
  snapshot->addToSnapshot(&event);
  const auto* object = snapshot->ticker_orders_[2][17];
  require(object && object->price_ == 100 && object->qty_ == 5 && object->priority_ == 9,
          "ADD snapshot state differs");
  require(snapshot->order_pool_.available() + 1 == initial_free,
          "ADD did not consume exactly one pool slot");
  event.seq_num_ = 2;
  event.me_market_update_.type_ = MarketUpdateType::MODIFY;
  event.me_market_update_.price_ = 101;
  event.me_market_update_.qty_ = 3;
  snapshot->addToSnapshot(&event);
  require(snapshot->ticker_orders_[2][17] == object && object->price_ == 101 && object->qty_ == 3 &&
              object->priority_ == 9 && snapshot->order_pool_.available() + 1 == initial_free,
          "MODIFY changed object identity, quantity, priority or pool slots");
  event.seq_num_ = 3;
  event.me_market_update_.type_ = MarketUpdateType::CANCEL;
  snapshot->addToSnapshot(&event);
  require(!snapshot->ticker_orders_[2][17] && snapshot->order_pool_.available() == initial_free &&
              snapshot->last_inc_seq_num_ == 3,
          "CANCEL left stale state or sequence");
}

// 子进程在对象构造前创建，避免复制带运行中日志线程的对象。
void expect_failure(const std::function<void()>& operation, const char* diagnostic) {
  int channel[2];
  require(::pipe(channel) == 0, "death-test pipe failed");
  const auto child = ::fork();
  require(child >= 0, "death-test fork failed");
  if (child == 0) {
    ::close(channel[0]);
    ::dup2(channel[1], STDERR_FILENO);
    ::close(channel[1]);
    operation();
    _exit(0);
  }
  ::close(channel[1]);
  std::string error;
  char bytes[512];
  for (ssize_t length; (length = ::read(channel[0], bytes, sizeof(bytes))) > 0;)
    error.append(bytes, static_cast<size_t>(length));
  ::close(channel[0]);
  int status = 0;
  require(::waitpid(child, &status, 0) == child, "death-test wait failed");
  require(WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE &&
              error.find("ASSERT : ") != std::string::npos &&
              error.find(diagnostic) != std::string::npos,
          "invalid state did not fail closed with the expected diagnostic");
}

void failure_cases() {
  expect_failure(
      [] {
        ClientRequestLFQueue requests(2);
        ClientResponseLFQueue responses(2);
        OrderServer server(&requests, &responses, "lo", 0);
        responses.try_push({ClientResponseType::ACCEPTED, 7, 0, 1, 1, Side::BUY, 100, 0, 1});
        server.run();
      },
      "Dont have a TCPSocket for ClientId:7");
  expect_failure(
      [] {
        MDPMarketUpdateLFQueue updates(2);
        auto snapshot = make_snapshot(updates);
        MDPMarketUpdate event{1, {MarketUpdateType::ADD, 17, 2, Side::BUY, 100, 5, 9}};
        snapshot->addToSnapshot(&event);
        event.seq_num_ = 2;
        snapshot->addToSnapshot(&event);
      },
      "but order already exists:");
  for (const auto type : {MarketUpdateType::MODIFY, MarketUpdateType::CANCEL}) {
    expect_failure(
        [type] {
          MDPMarketUpdateLFQueue updates(2);
          auto snapshot = make_snapshot(updates);
          MDPMarketUpdate event{1, {type, 17, 2, Side::BUY, 100, 5, 9}};
          snapshot->addToSnapshot(&event);
        },
        "but order does not exist.");
  }
}

int main() {
  try {
    logger_bytes_case();
    response_state_case();
    fifo_state_case();
    snapshot_state_case();
    failure_cases();
    std::cout << "PASS logger output bytes; response FIFO/epoch/business fields; "
                 "singleton/stable/backpressure FIFO; "
                 "snapshot ADD/MODIFY/CANCEL/pool; "
                 "four fail-closed diagnostics\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
