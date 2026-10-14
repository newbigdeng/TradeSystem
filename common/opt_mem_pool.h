#pragma once
#include "mem_pool.h"

namespace OptCommon {
// 复用同一份生命周期和耗尽处理，避免优化版本的语义分叉。
template <class T>
using OptMemPool = Common::MemPool<T>;
}
