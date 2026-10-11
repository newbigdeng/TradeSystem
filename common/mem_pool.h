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
  // 单线程管理的固定容量对象池：启动时分配存储，使用时构造 T，归还时析构 T。
  // 池的存储不扩容；T 自己的构造/析构仍可能分配内存或执行其他操作。
  template<class T>
  class MemPool final {
    struct Block {
      alignas(T) std::byte storage[sizeof(T)]; // 原始存储，满足 T 的对齐要求。
      bool used = false;                     // 保留重复释放检查和析构清理依据。
    };

    // 当前业务容量不超过 2^32-1；4 字节索引比 size_t 在 64 位平台上节省一半。
    using Index = uint32_t;
    static size_t checkedCapacity(size_t capacity) {
      if (!capacity || capacity > UINT32_MAX)
        throw std::invalid_argument("pool capacity must be in [1, UINT32_MAX]");
      return capacity;
    }

  public:
    explicit MemPool(size_t capacity)
        : blocks_(checkedCapacity(capacity)), free_(capacity) {
      // 先验证，再分配。倒序填充使首次分配从槽 0 开始。
      for (size_t i = 0; i < capacity; ++i)
        free_[i] = static_cast<Index>(capacity - i - 1);
    }

    template<class... Args>
    T *allocate(Args&&... args) {
      if (free_.empty()) return nullptr; // 耗尽时由业务决定拒绝或停止，不能覆写活对象。
      const auto index = free_.back();
      // 构造成功后才弹出索引；T 的构造函数抛异常时，池的空闲数量不变。
      auto *object = new(blocks_[index].storage) T(std::forward<Args>(args)...);
      free_.pop_back();
      blocks_[index].used = true;
      return object;
    }

    void deallocate(const T *object) noexcept {
      const auto address = reinterpret_cast<uintptr_t>(object);
      const auto base = reinterpret_cast<uintptr_t>(blocks_.data());
      // 先检查地址范围与槽位起点，再访问标记，防止外来或槽内偏移指针。
      ASSERT(address >= base && address < base + blocks_.size() * sizeof(Block)
             && (address - base) % sizeof(Block) == 0, "foreign pool pointer");
      const auto index = (address - base) / sizeof(Block);
      ASSERT(blocks_[index].used, "double pool release");
      std::destroy_at(const_cast<T*>(object));
      blocks_[index].used = false;
      free_.push_back(static_cast<Index>(index)); // LIFO 复用，容量已预留，不扩容。
    }

    ~MemPool() {
      // 原始字节数组不会自动析构 T，需要清理尚未归还的活对象。
      for (auto &block : blocks_)
        if (block.used)
          std::destroy_at(std::launder(reinterpret_cast<T*>(block.storage)));
    }

    size_t available() const noexcept { return free_.size(); }
    MemPool(const MemPool &) = delete;
    MemPool &operator=(const MemPool &) = delete;

  private:
    std::vector<Block> blocks_; // 地址在池存活期间稳定，归还之后不能继续使用旧对象。
    std::vector<Index> free_;   // 只记录空闲槽，不跨线程共享。
  };
}
