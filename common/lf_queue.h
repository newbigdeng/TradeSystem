#pragma once

#include <iostream>
#include <vector>
#include <atomic>

#include "macros.h"

namespace Common {
  template<typename T>
  class LFQueue final {
  private:
    // 底层容器，按FIFO顺序访问数据
    std::vector<T> store_;

    // 写索引和读索引，用于跟踪下一个要写入和读取的索引
    alignas(64) std::atomic<size_t> write_index_ = {0};
    alignas(64) std::atomic<size_t> read_index_ = {0};
  public:
    explicit LFQueue(std::size_t num_elems) :
        store_(num_elems, T()) /* 预分配 */ {
    }

    // 生产者：尝试写入
    bool try_push(const T& item) {
      size_t w = write_index_.load(std::memory_order_relaxed);
      size_t r = read_index_.load(std::memory_order_acquire);  // 获取最新读索引
      if (w - r >= store_.size()) return false; // 队列满
      store_[w % store_.size()] = item; //写入
      write_index_.store(w + 1, std::memory_order_release);   // 发布数据
      return true;
  }
  // 新增移动版本
  bool try_push(T&& item) {
      size_t w = write_index_.load(std::memory_order_relaxed);
      size_t r = read_index_.load(std::memory_order_acquire);
      if (w - r >= store_.size()) return false;
      store_[w % store_.size()] = std::move(item);   // 调用移动赋值
      write_index_.store(w + 1, std::memory_order_release);
      return true;
  }

  // 消费者：尝试读取
  bool try_pop(T& item) {
      size_t r = read_index_.load(std::memory_order_relaxed);
      size_t w = write_index_.load(std::memory_order_acquire); // 获取最新写索引
      if (r == w) return false; // 队列空
      item = std::move(store_[r % store_.size()]); // 读取
      read_index_.store(r + 1, std::memory_order_release);    // 释放槽位
      return true;
  }
  size_t size() const {
    return write_index_.load(std::memory_order_relaxed) - read_index_.load(std::memory_order_relaxed);
  }

    // 禁用默认构造函数、复制构造函数、移动构造函数和赋值运算符
    LFQueue() = delete;
    LFQueue(const LFQueue &) = delete;
    LFQueue(const LFQueue &&) = delete;
    LFQueue &operator=(const LFQueue &) = delete;
    LFQueue &operator=(const LFQueue &&) = delete;
  };
}
