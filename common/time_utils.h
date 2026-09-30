#pragma once

#include <string>
#include <chrono>
#include <ctime>

#include "perf_utils.h"

namespace Common {
  /// Represent a nanosecond timestamp.
  typedef int64_t Nanos;

  /// Convert between nanos, micros, millis and secs.
  constexpr Nanos NANOS_TO_MICROS = 1000;
  constexpr Nanos MICROS_TO_MILLIS = 1000;
  constexpr Nanos MILLIS_TO_SECS = 1000;
  constexpr Nanos NANOS_TO_MILLIS = NANOS_TO_MICROS * MICROS_TO_MILLIS;
  constexpr Nanos NANOS_TO_SECS = NANOS_TO_MILLIS * MILLIS_TO_SECS;

  /// Get current nanosecond timestamp.
  inline auto getCurrentNanos() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  }
  inline auto getMonotonicNanos() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  /// Format current timestamp to a human readable string.
  /// String formatting is inefficient.
  inline std::string getCurrentTimeStr() {
    const auto clock = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(clock);

    char calendar[26],nanos_str[24];
    ctime_r(&time,calendar);
    snprintf(nanos_str,sizeof(nanos_str),"%.8s.%09lld",calendar+11,
             static_cast<long long>(std::chrono::duration_cast<std::chrono::nanoseconds>(clock.time_since_epoch()).count()%NANOS_TO_SECS));
    return nanos_str;
  }
  // Compatibility for single-owner examples and preserved measurement probes.
  inline auto& getCurrentTimeStr(std::string* time_str) {*time_str=getCurrentTimeStr();return *time_str;}
}
