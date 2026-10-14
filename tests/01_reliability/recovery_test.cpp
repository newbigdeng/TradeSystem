#include <chrono>
#include <random>
#include <stdexcept>
#include "trading/market_data/market_recovery.h"
#include "common/market_protocol.h"
#include <tuple>
using U = Exchange::MDPMarketUpdate;
using E = Exchange::MEMarketUpdate;
using T = Exchange::MarketUpdateType;

void require_recovery(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}

std::vector<U> cycle(uint64_t identity, uint64_t watermark = 42) {
  std::vector<E> orders;
  for (uint64_t id = 1; id <= 64; ++id)
    orders.push_back({T::ADD, id, Common::TickerId(id % 8), Common::Side::BUY, 100, 1, id});
  const auto hash = Common::bookHash(orders);
  std::vector<U> frames;
  auto push = [&](E e) { frames.push_back({frames.size(), e, identity, watermark, hash}); };
  push({T::SNAPSHOT_START, watermark});
  for (Common::TickerId ticker = 0; ticker < 8; ++ticker)
    push({T::CLEAR, Common::OrderId_INVALID, ticker});
  for (const auto& order : orders)
    push(order);
  push({T::SNAPSHOT_END, watermark});
  return frames;
}

void inject(Trading::MarketRecovery& r, const std::vector<U>& frames) {
  for (const auto& frame : frames)
    r.ingest(true, frame);
}

// 直接对照重放后的字段，避免用第二次哈希代替业务状态检查。
auto replay(const std::vector<E>& events) {
  using Key = std::pair<Common::TickerId, Common::OrderId>;
  std::map<Key, std::tuple<Common::Side, Common::Price, Common::Qty, Common::Priority>> state;
  for (const auto& e : events) {
    const Key key{e.ticker_id_, e.order_id_};
    if (e.type_ == T::CLEAR) {
      for (auto it = state.begin(); it != state.end();) {
        if (it->first.first == e.ticker_id_)
          it = state.erase(it);
        else
          ++it;
      }
    } else if (e.type_ == T::ADD)
      state[key] = {e.side_, e.price_, e.qty_, e.priority_};
    else if (e.type_ == T::MODIFY)
      std::get<2>(state.at(key)) = e.qty_;
    else if (e.type_ == T::CANCEL)
      state.erase(key);
  }
  return state;
}

void recovery_boundaries() {
  // 所有公开类型可解码；内部结束标记及未知类型不得从网络进入。
  for (auto type : {T::CLEAR, T::ADD, T::MODIFY, T::CANCEL, T::TRADE, T::SNAPSHOT_START,
                    T::SNAPSHOT_END, T::INVALID, T::RECOVERY_COMMIT, static_cast<T>(99)}) {
    U frame, decoded;
    frame.me_market_update_.type_ = type;
    const auto bytes = Common::Wire::encode(frame);
    const bool public_type = type >= T::CLEAR && type <= T::SNAPSHOT_END;
    require_recovery(Common::Wire::decode(bytes.data(), decoded) == public_type,
                     "network market type boundary differs");
  }
  auto frames = cycle(1);
  const auto original = frames[9].me_market_update_;
  // 每个业务字段都参与重复帧对照，不能因诊断文本相同而忽略冲突。
  std::vector<E> conflicts(7, original);
  conflicts[0].type_ = T::MODIFY;
  conflicts[1].order_id_ += 64;
  conflicts[2].ticker_id_ = 0;
  conflicts[3].side_ = Common::Side::SELL;
  conflicts[4].price_ += 1;
  conflicts[5].qty_ += 1;
  conflicts[6].priority_ += 1;
  for (const auto& event : conflicts) {
    Trading::MarketRecovery r;
    inject(r, frames);
    auto duplicate = frames[9];
    duplicate.me_market_update_ = event;
    r.ingest(true, duplicate);
    require_recovery(r.invalid_ == 1 && !r.build(), "conflicting snapshot event accepted");
    inject(r, cycle(2));
    require_recovery(bool(r.build()), "new cycle failed after duplicate conflict");
  }
  for (bool hash : {false, true}) {
    Trading::MarketRecovery r;
    inject(r, frames);
    auto duplicate = frames[9];
    if (hash)
      ++duplicate.state_hash_;
    else
      ++duplicate.watermark_;
    r.ingest(true, duplicate);
    require_recovery(r.invalid_ == 1 && !r.build(), "conflicting snapshot metadata accepted");
  }
  Trading::MarketRecovery hole;
  for (size_t i = 0; i < frames.size(); ++i)
    if (i != 10)
      hole.ingest(true, frames[i]);
  for (int i = 0; i < 8; ++i)
    require_recovery(!hole.build(), "missing sequence accepted");
  hole.ingest(true, frames[10]);
  require_recovery(bool(hole.build()), "late snapshot frame failed");
  Trading::MarketRecovery bad_hash;
  auto broken = frames;
  for (auto& frame : broken)
    frame.state_hash_ ^= 1;
  inject(bad_hash, broken);
  require_recovery(!bad_hash.build() && bad_hash.hash_failures_ == 1,
                   "snapshot state corruption accepted");
  Trading::MarketRecovery transitions;
  inject(transitions, frames);
  transitions.ingest(false, {43, {T::ADD, 65, 0, Common::Side::BUY, 100, 3, 65}});
  transitions.ingest(false,
                     {44, {T::TRADE, Common::OrderId_INVALID, 0, Common::Side::BUY, 100, 1}});
  transitions.ingest(false, {45, {T::MODIFY, 65, 0, Common::Side::BUY, 100, 2, 65}});
  auto batch = transitions.build();
  require_recovery(batch && batch->watermark == 45, "TRADE/MODIFY sequence rejected");
  require_recovery(std::get<2>(replay(batch->events).at({0, 65})) == 2,
                   "TRADE or MODIFY double subtracted quantity");
  transitions.ingest(false, {46, {T::CANCEL, 65, 0, Common::Side::BUY, 100, 0, 65}});
  batch = transitions.build();
  require_recovery(batch && replay(batch->events).size() == 64, "CANCEL retained order");
  Trading::MarketRecovery invalid;
  inject(invalid, frames);
  invalid.ingest(false, {43, {T::MODIFY, 1, 1, Common::Side::BUY, 100, 0, 1}});
  require_recovery(!invalid.build(), "zero quantity MODIFY accepted");
  Trading::MarketRecovery inc_conflict;
  inject(inc_conflict, frames);
  U incremental{43, {T::ADD, 65, 0, Common::Side::BUY, 100, 1, 65}};
  inc_conflict.ingest(false, incremental);
  ++incremental.me_market_update_.qty_;
  inc_conflict.ingest(false, incremental);
  require_recovery(
      inc_conflict.invalid_ == 1 && inc_conflict.cached() == 0 && !inc_conflict.build(),
      "conflicting incremental accepted");
}

void test() {
  recovery_boundaries();
  auto frames = cycle(1);
  Trading::MarketRecovery good;
  std::mt19937 rng(991);
  auto shuffled = frames;
  std::shuffle(shuffled.begin(), shuffled.end(), rng);
  inject(good, shuffled);
  good.ingest(true, frames[3]);
  auto batch = good.build();
  require_recovery(batch && batch->events.size() == 72 && batch->watermark == 42,
                   "complete reordered/duplicated snapshot rejected");
  require_recovery(good.duplicates_ == 1, "snapshot duplicate not counted");
  good.commit(*batch);
  require_recovery(!good.recovering() && good.expected() == 43, "recovery commit failed");
  U duplicate{42, {T::TRADE, Common::OrderId_INVALID, 0, Common::Side::BUY, 100, 1}};
  require_recovery(!good.liveCandidate(duplicate) && !good.recovering(),
                   "old incremental triggered false gap");
  duplicate.seq_num_ = 44;
  require_recovery(!good.liveCandidate(duplicate) && good.recovering(),
                   "incremental gap kept market live");
  for (size_t missing : {size_t(0), size_t(5), frames.size() - 1}) {
    Trading::MarketRecovery r;
    auto incomplete = frames;
    incomplete.erase(incomplete.begin() + missing);
    for (size_t i = 0; i < incomplete.size(); ++i)
      incomplete[i].seq_num_ = i;
    inject(r, incomplete);
    require_recovery(!r.build(), "snapshot missing START/CLEAR/END accepted");
  }
  Trading::MarketRecovery mixed;
  auto newer = cycle(2);
  for (size_t i = 0; i < frames.size(); ++i)
    mixed.ingest(true, i % 2 ? newer[i] : frames[i]);
  require_recovery(!mixed.build(), "interleaved snapshot cycles mixed");
  Trading::MarketRecovery corrupt;
  auto broken = frames;
  broken.back().state_hash_ ^= 1;
  inject(corrupt, broken);
  require_recovery(!corrupt.build(), "bad hash accepted");
  inject(corrupt, cycle(2));
  require_recovery(bool(corrupt.build()), "new complete cycle did not repair previous bad hash");
  Trading::MarketRecovery gap;
  U first{43, {T::ADD, 65, 0, Common::Side::BUY, 100, 1, 65}},
      second{44, {T::MODIFY, 65, 0, Common::Side::BUY, 100, 2, 65}};
  gap.ingest(false, second);
  inject(gap, frames);
  require_recovery(!gap.build(), "gap after snapshot watermark accepted");
  gap.ingest(false, first);
  gap.ingest(false, first);
  batch = gap.build();
  require_recovery(batch && batch->watermark == 44 && batch->events.size() == 74,
                   "late missing incremental did not repair gap");
  std::vector<E> expected;
  for (const auto& frame : frames)
    if (frame.me_market_update_.type_ == T::ADD)
      expected.push_back(frame.me_market_update_);
  auto extra = first.me_market_update_;
  extra.qty_ = 2;
  expected.push_back(extra);
  require_recovery(replay(batch->events) == replay(expected),
                   "snapshot plus continuous incrementals rebuilt a different state");
  Trading::MarketRecovery bounded(10);
  inject(bounded, frames);
  require_recovery(bounded.overflows_ > 0 && bounded.cached() <= 10 && !bounded.build(),
                   "cache overflow was unbounded or marked live");
  // 128 槽中预占 54 槽，只剩 74；含 COMMIT 的 75 条恢复批次必须全部拒绝。
  Common::LFQueue<E> output(128);
  for (size_t i = 0; i < 54; ++i)
    require_recovery(output.try_push(first.me_market_update_), "prefill failed");
  batch->events.push_back({T::RECOVERY_COMMIT});
  require_recovery(!output.try_push_batch(batch->events) && output.size() == 54,
                   "full output published a partial recovery");
  E event;
  for (size_t i = 0; i < 54; ++i)
    require_recovery(output.try_pop(event), "prefill drain failed");
  require_recovery(!output.try_push_batch(std::vector<E>(129)), "oversized batch accepted");
  require_recovery(output.try_push_batch(batch->events) && output.size() == 75,
                   "complete recovery batch failed");
  for (const auto& expected_event : batch->events) {
    require_recovery(output.try_pop(event) && event.toString() == expected_event.toString(),
                     "recovery batch reordered");
  }
  require_recovery(!output.peek(), "recovery output not drained");
  require_recovery(output.try_push_batch(std::vector<E>(128)) && output.size() == 128,
                   "exact-capacity batch failed");
  Common::LFQueue<int> wrapped(4);
  wrapped.try_push(1);
  wrapped.try_push(2);
  wrapped.try_push(3);
  int n;
  wrapped.try_pop(n);
  wrapped.try_pop(n);
  require_recovery(wrapped.try_push_batch({4, 5, 6}), "wrapped batch failed");
  for (int want : {3, 4, 5, 6})
    require_recovery(wrapped.try_pop(n) && n == want, "wrapped batch reordered");
  const auto bytes = Common::Wire::encode(frames.back());
  U decoded;
  require_recovery(Common::Wire::decode(bytes.data(), decoded) && decoded.snapshot_cycle_ == 1 &&
                       decoded.watermark_ == 42 && decoded.state_hash_ == frames.back().state_hash_,
                   "market metadata wire roundtrip failed");
}

int main(int argc, char** argv) {
  try {
    if (argc > 1 && std::string(argv[1]) == "benchmark") {
      Trading::MarketRecovery recovery;
      auto frames = cycle(1);
      constexpr uint64_t count = 1000;
      uint64_t checksum = 0;
      const auto begin = std::chrono::steady_clock::now();
      for (uint64_t round = 0; round < count; ++round) {
        recovery.restart();
        for (auto& frame : frames)
          frame.snapshot_cycle_ = round + 1;
        inject(recovery, frames);
        const auto batch = recovery.build();
        require_recovery(bool(batch), "valid recovery benchmark failed");
        checksum += batch->events.size();
        recovery.commit(*batch);
      }
      const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - begin)
                          .count();
      require_recovery(checksum == count * 72, "recovery benchmark count differs");
      std::cout << "{\"mode\":\"recovery\",\"operations\":" << count << ",\"elapsed_ns\":" << ns
                << ",\"ns_per_operation\":" << double(ns) / count << ",\"checksum\":" << checksum
                << "}\n";
    } else {
      test();
      std::cout
          << "PASS recovery fault matrix, bounded cache, state hash and atomic output admission\n";
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
