#pragma once

#include <string>
#include <chrono>
#include <ctime>

#include "perf_utils.h"

namespace Common {
  /// 表示纳秒时间戳。
  typedef int64_t Nanos;

  /// 在纳秒、微秒、毫秒和秒之间进行转换。
  constexpr Nanos NANOS_TO_MICROS = 1000;
  constexpr Nanos MICROS_TO_MILLIS = 1000;
  constexpr Nanos MILLIS_TO_SECS = 1000;
  constexpr Nanos NANOS_TO_MILLIS = NANOS_TO_MICROS * MICROS_TO_MILLIS;
  constexpr Nanos NANOS_TO_SECS = NANOS_TO_MILLIS * MILLIS_TO_SECS;

  /// 获取当前纳秒时间戳。
  inline auto getCurrentNanos() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  }

  /// 将当前时间戳格式化为人类可读的字符串。
  /// 字符串格式化效率较低。
  inline auto& getCurrentTimeStr(std::string* time_str) {
    const auto clock = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(clock);

    char nanos_str[24];
    sprintf(nanos_str, "%.8s.%09ld", ctime(&time) + 11, std::chrono::duration_cast<std::chrono::nanoseconds>(clock.time_since_epoch()).count() % NANOS_TO_SECS);
    time_str->assign(nanos_str);

    return *time_str;
  }
}