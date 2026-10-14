#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <concepts>
#include <stdexcept>
#include <vector>
#include "macros.h"

namespace Common {
// 常量容量用此标签在编译期检查；运行时容量仍由构造函数检查。
template <size_t Capacity>
concept PowerOfTwoQueueCapacity = std::has_single_bit(Capacity) && Capacity <= SIZE_MAX / 2;

template <size_t Capacity>
  requires PowerOfTwoQueueCapacity<Capacity>
struct QueueCapacity final {};

// 单生产者、单消费者队列。若多个线程轮流生产，调用方必须在整个生产操作
// 外部加锁并建立同步（Logger 就采用这种方式），不能并发调用生产接口。
// 存储启动时在堆上分配，避免把业务大队列直接放进线程栈。
template <class T>
class LFQueue final {
  static size_t checkedCapacity(size_t capacity) {
    if (!std::has_single_bit(capacity) || capacity > SIZE_MAX / 2)
      throw std::invalid_argument("queue capacity must be a power of two, at most SIZE_MAX/2");
    return capacity;
  }

 public:
  explicit LFQueue(size_t capacity)
      : store_(checkedCapacity(capacity)), mask_(capacity - 1), cursor_mask_(capacity * 2 - 1) {}

  template <size_t Capacity>
  explicit LFQueue(QueueCapacity<Capacity>) : LFQueue(Capacity) {}

  // 满时不写入，调用方保留消息后重试，或执行业务规定的拒绝处理。
  bool try_push(const T& value) noexcept {
    const auto tail = tail_.load(std::memory_order_relaxed);  // 自己的游标。
    const auto used = distance(tail, head_.load(std::memory_order_acquire));
    if (used == store_.size()) {
      recordFull();
      return false;
    }
    // acquire head：消费者已用完槽位；release tail：发布完整写入的元素。
    store_[tail & mask_] = value;
    tail_.store(next(tail), std::memory_order_release);
    if (used + 1 > high_watermark_.load(std::memory_order_relaxed))
      high_watermark_.store(used + 1, std::memory_order_relaxed);
    return true;
  }

  // 整批容量检查通过后才写，一次 release 发布；失败时一个元素也不写。
  // 这保证批次的队列准入完整，不等于消费者一次调用就处理完整个业务批次。
  bool try_push_batch(const std::vector<T>& values) noexcept {
    if (values.empty())
      return true;  // 没有消息时无需触碰共享游标。
    const auto tail = tail_.load(std::memory_order_relaxed);
    const auto used = distance(tail, head_.load(std::memory_order_acquire));
    if (values.size() > store_.size() - used) {
      recordFull();
      return false;
    }
    auto write = tail;
    for (const auto& value : values) {
      store_[write & mask_] = value;
      write = next(write);
    }
    tail_.store(write, std::memory_order_release);
    if (used + values.size() > high_watermark_.load(std::memory_order_relaxed))
      high_watermark_.store(used + values.size(), std::memory_order_relaxed);
    return true;
  }

  // 消费者借用队首；空队列返回 nullptr。pop 后槽位可能被生产者覆写，
  // 需要在 pop 后继续使用的内容必须先复制。无需再调用 size 判断非空。
  const T* peek() const noexcept {
    const auto head = head_.load(std::memory_order_relaxed);
    return head == tail_.load(std::memory_order_acquire) ? nullptr : &store_[head & mask_];
  }

  void pop() noexcept {
    const auto head = head_.load(std::memory_order_relaxed);
    ASSERT(head != tail_.load(std::memory_order_acquire), "pop on empty SPSC queue");
    head_.store(next(head), std::memory_order_release);  // 读取完成后才归还槽位。
  }

  bool try_pop(T& value) noexcept {
    const auto head = head_.load(std::memory_order_relaxed);
    if (head == tail_.load(std::memory_order_acquire))
      return false;
    value = store_[head & mask_];
    head_.store(next(head), std::memory_order_release);
    return true;
  }

  // 两个游标不是原子快照，只作诊断；写入是否成功以 try_push 返回值为准。
  size_t size() const noexcept {
    const auto head = head_.load(std::memory_order_acquire);
    return std::min(distance(tail_.load(std::memory_order_acquire), head), store_.size());
  }

  size_t capacity() const noexcept {
    return store_.size();
  }

  uint64_t full_count() const noexcept {
    return full_count_.load(std::memory_order_relaxed);
  }

  size_t high_watermark() const noexcept {
    return high_watermark_.load(std::memory_order_relaxed);
  }

  LFQueue(const LFQueue&) = delete;
  LFQueue& operator=(const LFQueue&) = delete;

 private:
  // 游标在 [0, 2*capacity) 回绕，下标在 [0, capacity) 回绕。
  // 两倍周期区分空和满，capacity 个槽全部可用，capacity=1 也成立。
  size_t next(size_t i) const noexcept {
    return (i + 1) & cursor_mask_;
  }

  size_t distance(size_t tail, size_t head) const noexcept {
    return (tail - head) & cursor_mask_;
  }

  // 满队列计数只有生产者写；外部线程只读原子发布值。
  void recordFull() noexcept {
    full_count_.store(full_count_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
  }

  std::vector<T> store_;
  const size_t mask_;
  const size_t cursor_mask_;
  alignas(64) std::atomic<size_t> head_{0};  // 分开写热点，减小伪共享。
  alignas(64) std::atomic<size_t> tail_{0};
  std::atomic<uint64_t> full_count_{0};
  std::atomic<size_t> high_watermark_{0};
};
}
