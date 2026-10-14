#pragma once
#include <cstdint>

// 这里只控制可关闭的诊断计时记录。
// 持久化订单日志和业务、错误日志不受此开关影响。
#ifndef TRADE_PERF_TRACE_EVERY
#define TRADE_PERF_TRACE_EVERY 1
#endif
#if TRADE_PERF_TRACE_EVERY < 0
#error "TRADE_PERF_TRACE_EVERY must be nonnegative"
#endif

namespace Common {
/// 读取诊断用 TSC 计数；未经标定，不能当作纳秒或核心周期。
inline auto rdtsc() noexcept {
  unsigned int lo, hi;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}
}

#if TRADE_PERF_TRACE_EVERY == 0
// 关闭计时后不读时钟，也不求值日志参数。
#define START_MEASURE(TAG) \
  do {                     \
  } while (false)
#define END_MEASURE(TAG, LOGGER) \
  do {                           \
  } while (false)
#define TTT_MEASURE(TAG, LOGGER) \
  do {                           \
  } while (false)
#elif TRADE_PERF_TRACE_EVERY == 1
/// 开始记录 TSC 计数；该数值不是标定后的纳秒。
#define START_MEASURE(TAG) const auto TAG = Common::rdtsc()

/// 用 rdtsc() 结束计时；当前作用域必须已有名为 TAG 的起始计数。
#define END_MEASURE(TAG, LOGGER)                                                  \
  do {                                                                            \
    const auto end = Common::rdtsc();                                             \
    LOGGER.log("% RDTSC " #TAG " %\n", Common::getCurrentTimeStr(), (end - TAG)); \
  } while (false)

/// 记录调用时的时间戳。
#define TTT_MEASURE(TAG, LOGGER)                                        \
  do {                                                                  \
    const auto TAG = Common::getCurrentNanos();                         \
    LOGGER.log("% TTT " #TAG " %\n", Common::getCurrentTimeStr(), TAG); \
  } while (false)

#else
// 每个调用点、每个所属线程各自维护确定性计数。
// 按调用次数抽取记录，不代表慢订单的统计分布。
#define START_MEASURE(TAG)                             \
  static thread_local uint64_t TAG##_sample_count = 0; \
  const auto TAG =                                     \
      (++TAG##_sample_count % TRADE_PERF_TRACE_EVERY == 0) ? Common::rdtsc() : uint64_t{0}
#define END_MEASURE(TAG, LOGGER)                                                  \
  do {                                                                            \
    if (TAG) {                                                                    \
      const auto end = Common::rdtsc();                                           \
      LOGGER.log("% RDTSC " #TAG " %\n", Common::getCurrentTimeStr(), end - TAG); \
    }                                                                             \
  } while (false)
#define TTT_MEASURE(TAG, LOGGER)                                          \
  do {                                                                    \
    static thread_local uint64_t TAG##_sample_count = 0;                  \
    if (++TAG##_sample_count % TRADE_PERF_TRACE_EVERY == 0) {             \
      const auto TAG = Common::getCurrentNanos();                         \
      LOGGER.log("% TTT " #TAG " %\n", Common::getCurrentTimeStr(), TAG); \
    }                                                                     \
  } while (false)
#endif
