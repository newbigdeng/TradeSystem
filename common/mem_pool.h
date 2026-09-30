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
// Single-owner storage. Allocation constructs one T; release destroys it.
template<class T> class MemPool final {
  struct Block { alignas(T) std::byte storage[sizeof(T)]; bool used=false; };
 public:
  explicit MemPool(size_t capacity):blocks_(capacity),free_(capacity) {
    if(!capacity) throw std::invalid_argument("zero pool capacity");
    for(size_t i=0;i<capacity;++i) free_[i]=capacity-i-1;
  }
  template<class... Args> T *allocate(Args&&... args) {
    if(free_.empty()) return nullptr;
    const auto index=free_.back();
    auto *object=new(blocks_[index].storage) T(std::forward<Args>(args)...);
    free_.pop_back(); blocks_[index].used=true; return object;
  }
  void deallocate(const T *object) noexcept {
    const auto address=reinterpret_cast<uintptr_t>(object), base=reinterpret_cast<uintptr_t>(blocks_.data());
    ASSERT(address>=base && address<base+blocks_.size()*sizeof(Block) && (address-base)%sizeof(Block)==0,"foreign pool pointer");
    const auto index=(address-base)/sizeof(Block);
    ASSERT(blocks_[index].used,"double pool release");
    std::destroy_at(const_cast<T*>(object)); blocks_[index].used=false; free_.push_back(index);
  }
  ~MemPool() {
    for(auto &b:blocks_) if(b.used) std::destroy_at(std::launder(reinterpret_cast<T*>(b.storage)));
  }
  size_t available() const noexcept { return free_.size(); }
  MemPool(const MemPool &)=delete; MemPool &operator=(const MemPool &)=delete;
 private:
  std::vector<Block> blocks_; std::vector<size_t> free_;
};
}
