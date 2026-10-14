#pragma once

#include <string>
#include <chrono>
#include <ctime>

#include "perf_utils.h"

namespace Common {
/// 纳秒时间戳类型。
typedef int64_t Nanos;

/// 纳秒、微秒、毫秒和秒之间的换算常量。
constexpr Nanos NANOS_TO_MICROS = 1000;
constexpr Nanos MICROS_TO_MILLIS = 1000;
constexpr Nanos MILLIS_TO_SECS = 1000;
constexpr Nanos NANOS_TO_MILLIS = NANOS_TO_MICROS * MICROS_TO_MILLIS;
constexpr Nanos NANOS_TO_SECS = NANOS_TO_MILLIS * MILLIS_TO_SECS;

/// 读取当前纳秒时间戳。
inline auto getCurrentNanos() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

inline auto getMonotonicNanos() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/// 将当前时间格式化为便于阅读的字符串。
/// 字符串格式化有额外开销。
inline std::string getCurrentTimeStr() {
  const auto clock = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(clock);

  char calendar[26], nanos_str[24];
  ctime_r(&time, calendar);
  snprintf(
      nanos_str, sizeof(nanos_str), "%.8s.%09lld", calendar + 11,
      static_cast<long long>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(clock.time_since_epoch()).count() %
          NANOS_TO_SECS));
  return nanos_str;
}

// 为单线程示例和已有测量程序保留兼容入口。
inline auto& getCurrentTimeStr(std::string* time_str) {
  *time_str = getCurrentTimeStr();
  return *time_str;
}
}
