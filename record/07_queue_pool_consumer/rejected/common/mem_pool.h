#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>
#include "macros.h"

namespace Common {
// 单线程管理的固定容量对象池：启动时分配，借出时构造，归还时析构。
// 对象存储不扩容；T 自己的构造和析构仍可能分配内存。
template<class T>
class MemPool final {
  struct Block {
    // 将使用标记分开存放，避免每个槽因 bool 再补一轮对齐填充。
    alignas(T) std::byte storage[sizeof(T)];
  };

  using Index = uint32_t;
  static size_t checkedCapacity(size_t capacity) {
    if (!capacity || capacity > UINT32_MAX)
      throw std::invalid_argument("pool capacity must be in [1, UINT32_MAX]");
    return capacity;
  }

public:
  explicit MemPool(size_t capacity)
      : blocks_(checkedCapacity(capacity)), free_(capacity), used_(capacity, 0) {
    // 先验证，再分配。倒序填充使首次分配从槽 0 开始。
    for (size_t i = 0; i < capacity; ++i)
      free_[i] = static_cast<Index>(capacity - i - 1);
  }

  template<class... Args>
  T* allocate(Args&&... args) {
    if (free_.empty())
      return nullptr;
    const auto index = free_.back();
    // 构造成功后才借出槽；构造抛异常不改变空闲栈和使用标记。
    auto* object = new(blocks_[index].storage) T(std::forward<Args>(args)...);
    free_.pop_back();
    used_[index] = 1;
    return object;
  }

  void deallocate(const T* object) noexcept {
    const auto address = reinterpret_cast<uintptr_t>(object);
    const auto base = reinterpret_cast<uintptr_t>(blocks_.data());
    // 先检查范围和槽起点，不能用外来或槽内偏移指针访问使用标记。
    ASSERT(address >= base && address < base + blocks_.size() * sizeof(Block)
               && (address - base) % sizeof(Block) == 0,
           "foreign pool pointer");
    const auto index = (address - base) / sizeof(Block);
    ASSERT(used_[index], "double pool release");
    std::destroy_at(const_cast<T*>(object));
    used_[index] = 0;
    free_.push_back(static_cast<Index>(index)); // LIFO 复用，预留容量足够，不扩容。
  }

  ~MemPool() {
    for (size_t i = 0; i < blocks_.size(); ++i)
      if (used_[i])
        std::destroy_at(std::launder(reinterpret_cast<T*>(blocks_[i].storage)));
  }

  size_t available() const noexcept { return free_.size(); }
  MemPool(const MemPool&) = delete;
  MemPool& operator=(const MemPool&) = delete;

private:
  std::vector<Block> blocks_; // 地址在池存活期间稳定，归还后不能继续使用旧对象。
  std::vector<Index> free_;   // 单线程空闲索引栈。
  std::vector<uint8_t> used_; // 每槽一字节，保留重复释放检查和析构清理。
};
}
