#pragma once

namespace Common {
  /// 从TSC寄存器读取一个uint64_t值，代表已用的CPU时钟周期数
  inline auto rdtsc() noexcept {
    unsigned int lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t) hi << 32) | lo;
  }
}

/// 开始测量延迟，创建一个名为TAG的本地变量
#define START_MEASURE(TAG) const auto TAG = Common::rdtsc()

/// 结束测量延迟，记录延迟时间，并打印到日志
#define END_MEASURE(TAG, LOGGER)                                                              \
      do {                                                                                    \
        const auto end = Common::rdtsc();                                                     \
        LOGGER.log("% RDTSC "#TAG" %\n", Common::getCurrentTimeStr(&time_str_), (end - TAG)); \
      } while(false)

/// 记录当前时间戳，用于测量不同模块的延迟
#define TTT_MEASURE(TAG, LOGGER)                                                              \
      do {                                                                                    \
        const auto TAG = Common::getCurrentNanos();                                           \
        LOGGER.log("% TTT "#TAG" %\n", Common::getCurrentTimeStr(&time_str_), TAG);           \
      } while(false)
