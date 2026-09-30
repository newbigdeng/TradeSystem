#pragma once
#include "mem_pool.h"
namespace OptCommon {
// Keep one correct lifetime/exhaustion contract before attempting optimizations.
template<class T> using OptMemPool=Common::MemPool<T>;
}
