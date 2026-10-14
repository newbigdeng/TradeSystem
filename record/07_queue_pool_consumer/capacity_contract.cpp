#include <cstdint>
#include <type_traits>
#include "common/lf_queue.h"

// 常量容量在编译期拒绝 0、非 2 的幂和游标周期会溢出的值。
template <size_t N>
concept HasCapacityTag = requires { typename Common::QueueCapacity<N>; };

static_assert(HasCapacityTag<1> && HasCapacityTag<2> && HasCapacityTag<1024>);
static_assert(!HasCapacityTag<0> && !HasCapacityTag<3> && !HasCapacityTag<74>);
static_assert(!HasCapacityTag<SIZE_MAX / 2 + 1> && !HasCapacityTag<SIZE_MAX>);
static_assert(HasCapacityTag<(SIZE_MAX / 2 + 1) / 2>);
// 运行时容量接口继续保留；容量标签不能隐式退化成整数绕过约束。
static_assert(std::is_constructible_v<Common::LFQueue<uint64_t>, size_t>);
static_assert(std::is_constructible_v<Common::LFQueue<uint64_t>, Common::QueueCapacity<1>>);
static_assert(!std::is_convertible_v<Common::QueueCapacity<4>, size_t>);

int main() {}
