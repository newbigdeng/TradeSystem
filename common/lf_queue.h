#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <vector>
#include "macros.h"
namespace Common {
// One producer, one consumer. Indices wrap at 2*capacity, not SIZE_MAX;
// this preserves slot mapping for arbitrary capacities, including 1 and 3.
template<class T> class LFQueue final {
 public:
  explicit LFQueue(size_t capacity):store_(capacity),modulus_(capacity*2) {
    if(!capacity || capacity>SIZE_MAX/2) throw std::invalid_argument("invalid queue capacity");
  }
  bool try_push(const T &value) noexcept {
    const auto tail=tail_.load(std::memory_order_relaxed);
    const auto used=distance(tail,head_.load(std::memory_order_acquire));
    if(used==store_.size()) { full_count_.fetch_add(1,std::memory_order_relaxed); return false; }
    store_[tail%store_.size()]=value;
    tail_.store(next(tail),std::memory_order_release);
    if(used+1>high_watermark_.load(std::memory_order_relaxed)) high_watermark_.store(used+1,std::memory_order_relaxed);
    return true;
  }
  bool try_pop(T &value) noexcept {
    const auto head=head_.load(std::memory_order_relaxed);
    if(head==tail_.load(std::memory_order_acquire)) return false;
    value=store_[head%store_.size()]; head_.store(next(head),std::memory_order_release); return true;
  }
  bool try_push_batch(const std::vector<T>& values) noexcept {
    const auto tail=tail_.load(std::memory_order_relaxed);
    const auto used=distance(tail,head_.load(std::memory_order_acquire));
    if(values.size()>store_.size()-used) {full_count_.fetch_add(1,std::memory_order_relaxed);return false;}
    auto write=tail;
    for(const auto& value:values) {store_[write%store_.size()]=value;write=next(write);}
    tail_.store(write,std::memory_order_release);
    if(used+values.size()>high_watermark_.load(std::memory_order_relaxed))high_watermark_.store(used+values.size(),std::memory_order_relaxed);
    return true;
  }
  // Consumer-only peek/commit retains a frame when an output buffer is full.
  const T *peek() const noexcept {
    const auto head=head_.load(std::memory_order_relaxed);
    return head==tail_.load(std::memory_order_acquire)?nullptr:&store_[head%store_.size()];
  }
  void pop() noexcept {
    const auto head=head_.load(std::memory_order_relaxed);
    ASSERT(head!=tail_.load(std::memory_order_acquire),"pop on empty SPSC queue");
    head_.store(next(head),std::memory_order_release);
  }
  // Diagnostic snapshot only; callers reserve slots through try_push/try_pop.
  size_t size() const noexcept {
    const auto head=head_.load(std::memory_order_acquire);
    return std::min(distance(tail_.load(std::memory_order_acquire),head),store_.size());
  }
  size_t capacity() const noexcept { return store_.size(); }
  uint64_t full_count() const noexcept { return full_count_.load(std::memory_order_relaxed); }
  size_t high_watermark() const noexcept { return high_watermark_.load(std::memory_order_relaxed); }
  LFQueue(const LFQueue &)=delete; LFQueue &operator=(const LFQueue &)=delete;
 private:
  size_t next(size_t i) const noexcept { return i+1==modulus_?0:i+1; }
  size_t distance(size_t tail,size_t head) const noexcept { return tail>=head?tail-head:modulus_-(head-tail); }
  std::vector<T> store_; size_t modulus_;
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
  std::atomic<uint64_t> full_count_{0}; std::atomic<size_t> high_watermark_{0};
};
}
