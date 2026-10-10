#pragma once
#include <cstdint>

// Only optional diagnostic timing records are controlled here. The durable
// order journal and ordinary business/error logs are independent of this mode.
#ifndef TRADE_PERF_TRACE_EVERY
#define TRADE_PERF_TRACE_EVERY 1
#endif
#if TRADE_PERF_TRACE_EVERY < 0
#error "TRADE_PERF_TRACE_EVERY must be nonnegative"
#endif

namespace Common {
  /// Read diagnostic TSC ticks. They are neither calibrated ns nor core cycles.
  inline auto rdtsc() noexcept {
    unsigned int lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((uint64_t) hi << 32) | lo;
  }
}

#if TRADE_PERF_TRACE_EVERY == 0
// Disabled instrumentation must not read a clock or evaluate logger arguments.
#define START_MEASURE(TAG) do {} while(false)
#define END_MEASURE(TAG, LOGGER) do {} while(false)
#define TTT_MEASURE(TAG, LOGGER) do {} while(false)
#elif TRADE_PERF_TRACE_EVERY == 1
/// Start diagnostic TSC measurement. TSC ticks are not calibrated nanoseconds.
#define START_MEASURE(TAG) const auto TAG = Common::rdtsc()

/// End latency measurement using rdtsc(). Expects a variable called TAG to already exist in the local scope.
#define END_MEASURE(TAG, LOGGER)                                                              \
      do {                                                                                    \
        const auto end = Common::rdtsc();                                                     \
        LOGGER.log("% RDTSC "#TAG" %\n", Common::getCurrentTimeStr(), (end - TAG)); \
      } while(false)

/// Log a current timestamp at the time this macro is invoked.
#define TTT_MEASURE(TAG, LOGGER)                                                              \
      do {                                                                                    \
        const auto TAG = Common::getCurrentNanos();                                           \
        LOGGER.log("% TTT "#TAG" %\n", Common::getCurrentTimeStr(), TAG);           \
      } while(false)

#else
// A deterministic counter belongs to each call site and each owning thread.
// This selects calls, not a representative statistical sample of slow orders.
#define START_MEASURE(TAG) \
  static thread_local uint64_t TAG##_sample_count = 0; \
  const auto TAG = (++TAG##_sample_count % TRADE_PERF_TRACE_EVERY == 0) ? Common::rdtsc() : uint64_t{0}
#define END_MEASURE(TAG, LOGGER) \
  do { \
    if (TAG) { \
      const auto end = Common::rdtsc(); \
      LOGGER.log("% RDTSC "#TAG" %\n", Common::getCurrentTimeStr(), end - TAG); \
    } \
  } while(false)
#define TTT_MEASURE(TAG, LOGGER) \
  do { \
    static thread_local uint64_t TAG##_sample_count = 0; \
    if (++TAG##_sample_count % TRADE_PERF_TRACE_EVERY == 0) { \
      const auto TAG = Common::getCurrentNanos(); \
      LOGGER.log("% TTT "#TAG" %\n", Common::getCurrentTimeStr(), TAG); \
    } \
  } while(false)
#endif
