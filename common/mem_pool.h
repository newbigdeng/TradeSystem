#pragma once

#include <cstdint>
#include <vector>
#include <string>

#include "macros.h"

namespace Common {
  template<typename T>
  class MemPool final {
  public:
    explicit MemPool(std::size_t num_elems) :
        store_(num_elems, {T(), true}) /* 预分配内存存储 */ {
      ASSERT(reinterpret_cast<const ObjectBlock *>(&(store_[0].object_)) == &(store_[0]), "T object should be first member of ObjectBlock.");
    }

    /// 分配一个新的类型的对象，使用placement new初始化对象，标记为已使用并返回对象
    template<typename... Args>
    T *allocate(Args... args) noexcept {
      auto obj_block = &(store_[next_free_index_]);
      ASSERT(obj_block->is_free_, "Expected free ObjectBlock at index:" + std::to_string(next_free_index_));
      T *ret = &(obj_block->object_);
      ret = new(ret) T(args...); // placement new.
      obj_block->is_free_ = false; // 标记为已使用

      updateNextFreeIndex();

      return ret;
    }

    /// 计算对象的内存索引，标记为已释放
    /// 对象的析构函数不会被调用
    auto deallocate(const T *elem) noexcept {
      const auto elem_index = (reinterpret_cast<const ObjectBlock *>(elem) - &store_[0]);
      ASSERT(elem_index >= 0 && static_cast<size_t>(elem_index) < store_.size(), "Element being deallocated does not belong to this Memory pool.");
      ASSERT(!store_[elem_index].is_free_, "Expected in-use ObjectBlock at index:" + std::to_string(elem_index));
      store_[elem_index].is_free_ = true;
    }

    /// 禁用默认构造函数、复制构造函数、移动构造函数和赋值运算符
    MemPool() = delete;
    MemPool(const MemPool &) = delete;
    MemPool(const MemPool &&) = delete;
    MemPool &operator=(const MemPool &) = delete;
    MemPool &operator=(const MemPool &&) = delete;
  private:
    /// 更新下一个可用的空闲块索引
    auto updateNextFreeIndex() noexcept {
      const auto initial_free_index = next_free_index_;
      while (!store_[next_free_index_].is_free_) {
        ++next_free_index_;
        if (UNLIKELY(next_free_index_ == store_.size())) { // 到达末尾，重置索引。这个情况应该很少发生。
          next_free_index_ = 0;
        }
        if (UNLIKELY(initial_free_index == next_free_index_)) {
          ASSERT(initial_free_index != next_free_index_, "Memory Pool out of space.");
        }
      }
    }

    /// 内存块结构体，包含对象和是否为空标志位
    struct ObjectBlock {
      T object_;
      bool is_free_ = true;
    };

    /// 内存池存储，使用std::vector存储ObjectBlock，在堆上分配内存
    /// 如果使用std::array，我们需要测试性能差异
    /// 使用栈上的std::array，性能可能更好，但是当内存池大小增加时，性能会下降
    std::vector<ObjectBlock> store_;

    size_t next_free_index_ = 0;
  };
}
