#pragma once
#include <map>
#include <optional>
#include <tuple>
#include "common/book_hash.h"

namespace Trading {
// 先验证一个完整快照，再续接连续增量；build 只产出候选，整批入队后才 commit。
class MarketRecovery {
 public:
  using Update = Exchange::MDPMarketUpdate;
  using Event = Exchange::MEMarketUpdate;
  using Type = Exchange::MarketUpdateType;

  struct Batch {
    std::vector<Event> events;
    uint64_t watermark;
  };

  explicit MarketRecovery(size_t limit = Common::ME_MAX_MARKET_UPDATES) : limit_(limit) {
    ASSERT(limit >= Common::ME_MAX_TICKERS + 2, "recovery cache too small");
  }

  // 当前周期作废，等待更新的快照；不提前推进实时序号。
  void restart() {
    recovering_ = true;
    snapshot_.clear();
    incremental_.clear();
    invalid_cycle_ = true;
  }

  bool recovering() const noexcept {
    return recovering_;
  }

  uint64_t expected() const noexcept {
    return expected_;
  }

  size_t cached() const noexcept {
    return snapshot_.size() + incremental_.size();
  }

  // 序号连续才能尝试入下游队列；只有投递成功后才 commitLive。
  bool liveCandidate(const Update& update) {
    if (recovering_)
      return false;
    if (update.seq_num_ == expected_)
      return true;
    if (update.seq_num_ < expected_) {
      ++duplicates_;
      return false;
    }
    ++gaps_;
    restart();
    return false;
  }

  void commitLive() {
    ++expected_;
  }

  // 按序号缓存，允许乱序到达；相同序号出现不同内容时拒绝整个周期。
  bool ingest(bool snapshot, const Update& update) {
    if (!recovering_)
      return false;
    if (snapshot) {
      if (!update.snapshot_cycle_ || update.snapshot_cycle_ < cycle_)
        return false;
      if (update.snapshot_cycle_ > cycle_) {
        cycle_ = update.snapshot_cycle_;
        snapshot_.clear();
        invalid_cycle_ = false;
        // 快照已覆盖的增量不再参与恢复。
        incremental_.erase(incremental_.begin(), incremental_.upper_bound(update.watermark_));
      }
      if (invalid_cycle_)
        return false;
    }

    auto& cache = snapshot ? snapshot_ : incremental_;
    // try_emplace 只查一次；重复帧不分配新节点，也不构造诊断字符串。
    const auto [found, inserted] = cache.try_emplace(update.seq_num_, update);
    if (!inserted) {
      const auto& old = found->second;
      const bool same = sameEvent(old.me_market_update_, update.me_market_update_) &&
                        (!snapshot || (old.watermark_ == update.watermark_ &&
                                       old.state_hash_ == update.state_hash_));
      if (!snapshot)
        ++duplicates_;
      if (!same) {
        if (snapshot) {
          invalid_cycle_ = true;
          snapshot_.clear();
        } else {
          restart();
        }
        ++invalid_;
        return false;
      }
      if (snapshot) {
        ++duplicates_;
        return false;
      }
    }
    if (cached() > limit_) {
      restart();
      ++overflows_;
      return false;
    }
    return true;
  }

  std::optional<Batch> build() {
    if (snapshot_.empty() || invalid_cycle_)
      return std::nullopt;
    const auto& first = snapshot_.begin()->second;
    const auto& last = snapshot_.rbegin()->second;
    if (first.seq_num_ != 0 || first.me_market_update_.type_ != Type::SNAPSHOT_START ||
        last.me_market_update_.type_ != Type::SNAPSHOT_END)
      return std::nullopt;
    const auto watermark = first.watermark_;
    if (last.watermark_ != watermark || first.me_market_update_.order_id_ != watermark ||
        last.me_market_update_.order_id_ != watermark || first.state_hash_ != last.state_hash_) {
      invalid_cycle_ = true;
      ++invalid_;
      return std::nullopt;
    }
    // map 中序号唯一且有序；从 0 开始时，尾序号等于帧数减一才可能完整。
    // 缺帧直接等待，避免每次重建临时订单簿。
    if (last.seq_num_ != snapshot_.size() - 1)
      return std::nullopt;
    const auto incremental_begin = incremental_.upper_bound(watermark);
    if (incremental_begin != incremental_.end() && incremental_begin->first != watermark + 1)
      return std::nullopt;

    std::array<bool, Common::ME_MAX_TICKERS> cleared{};
    State state;
    Batch batch{{}, watermark};
    batch.events.reserve(snapshot_.size() - 2 + incremental_.size());
    for (const auto& [id, wrapped] : snapshot_) {
      if (wrapped.watermark_ != watermark || wrapped.snapshot_cycle_ != cycle_ ||
          wrapped.state_hash_ != first.state_hash_)
        return std::nullopt;
      const auto& event = wrapped.me_market_update_;
      if (id == 0)
        continue;
      if (event.type_ == Type::SNAPSHOT_END) {
        if (id != last.seq_num_)
          return std::nullopt;
        continue;
      }
      if (event.ticker_id_ >= Common::ME_MAX_TICKERS)
        return std::nullopt;
      if (event.type_ == Type::CLEAR) {
        if (cleared[event.ticker_id_])
          return std::nullopt;
        cleared[event.ticker_id_] = true;
      } else if (event.type_ != Type::ADD || !cleared[event.ticker_id_]) {
        return std::nullopt;
      }
      if (!apply(state, event))
        return std::nullopt;
      batch.events.push_back(event);
    }
    if (!std::all_of(cleared.begin(), cleared.end(), [](bool value) { return value; }))
      return std::nullopt;

    // 保留发布端与接收端之间的一次快照校验，不再计算最终批次哈希。
    if (Common::bookHash(flatten(state)) != first.state_hash_) {
      ++hash_failures_;
      invalid_cycle_ = true;
      return std::nullopt;
    }
    uint64_t next = watermark + 1;
    for (auto it = incremental_begin; it != incremental_.end(); ++it) {
      if (it->first != next || !apply(state, it->second.me_market_update_))
        return std::nullopt;
      batch.events.push_back(it->second.me_market_update_);
      ++next;
    }
    batch.watermark = next - 1;
    return batch;
  }

  // 下游队列整批准入成功；消费线程仍须按 FIFO 应用到结束标记。
  void commit(const Batch& batch) {
    expected_ = batch.watermark + 1;
    recovering_ = false;
    snapshot_.clear();
    incremental_.clear();
    ++recoveries_;
  }

  uint64_t gaps_ = 0, duplicates_ = 0, overflows_ = 0, invalid_ = 0, hash_failures_ = 0,
           recoveries_ = 0;

 private:
  using State = std::array<std::map<Common::OrderId, Event>, Common::ME_MAX_TICKERS>;

  static bool sameEvent(const Event& a, const Event& b) noexcept {
    return std::tie(a.type_, a.order_id_, a.ticker_id_, a.side_, a.price_, a.qty_, a.priority_) ==
           std::tie(b.type_, b.order_id_, b.ticker_id_, b.side_, b.price_, b.qty_, b.priority_);
  }

  static std::vector<Event> flatten(const State& state) {
    size_t count = 0;
    for (const auto& book : state)
      count += book.size();
    std::vector<Event> events;
    events.reserve(count);
    for (const auto& book : state)
      for (const auto& [id, event] : book) {
        (void)id;
        events.push_back(event);
      }
    return events;
  }

  // TRADE 不扣簿；MODIFY 设置绝对剩余量；CANCEL 直接删除。
  static bool apply(State& state, const Event& event) {
    if (event.ticker_id_ >= Common::ME_MAX_TICKERS)
      return false;
    auto& orders = state[event.ticker_id_];
    if (event.type_ == Type::CLEAR) {
      orders.clear();
      return true;
    }
    if (event.type_ == Type::TRADE)
      return event.qty_ > 0 && event.qty_ != Common::Qty_INVALID && event.price_ > 0 &&
             event.price_ != Common::Price_INVALID &&
             (event.side_ == Common::Side::BUY || event.side_ == Common::Side::SELL);
    if (!event.order_id_ || event.order_id_ >= Common::ME_MAX_ORDER_IDS)
      return false;
    auto found = orders.find(event.order_id_);
    if (event.type_ == Type::ADD) {
      if (found != orders.end() || event.price_ <= 0 || event.price_ == Common::Price_INVALID ||
          !event.qty_ || event.qty_ == Common::Qty_INVALID || !event.priority_ ||
          event.priority_ == Common::Priority_INVALID ||
          (event.side_ != Common::Side::BUY && event.side_ != Common::Side::SELL))
        return false;
      orders.emplace(event.order_id_, event);
      return true;
    }
    // 修改和撤销只能作用于相同方向、相同价格的已有挂单。
    if (found == orders.end() || found->second.side_ != event.side_ ||
        found->second.price_ != event.price_)
      return false;
    if (event.type_ == Type::CANCEL) {
      orders.erase(found);
      return true;
    }
    if (event.type_ == Type::MODIFY && event.qty_ > 0 && event.qty_ != Common::Qty_INVALID) {
      found->second.qty_ = event.qty_;
      return true;
    }
    return false;
  }

  size_t limit_;
  bool recovering_ = true, invalid_cycle_ = false;
  uint64_t cycle_ = 0, expected_ = 1;
  std::map<uint64_t, Update> snapshot_, incremental_;
};
}
