#include <chrono>
#include <iostream>
#include <stdexcept>
#include "common/market_protocol.h"

// 独立调用解码器，避免编译器将固定输入的整个循环折叠成一个常数。
__attribute__((noinline)) bool decode_frame(const uint8_t* bytes, Exchange::MDPMarketUpdate& out) {
  return Common::Wire::decode(bytes, out);
}

int main() {
  using T = Exchange::MarketUpdateType;
  Exchange::MDPMarketUpdate input{43, {T::ADD, 1, 0, Common::Side::BUY, 100, 1, 1}}, output;
  const auto bytes = Common::Wire::encode(input);
  constexpr uint64_t count = 1000000;
  uint64_t checksum = 0;
  for (size_t i = 0; i < 10000; ++i)
    if (!decode_frame(bytes.data(), output))
      return 1;
  const auto start = std::chrono::steady_clock::now();
  for (uint64_t i = 0; i < count; ++i) {
    __asm__ volatile("" : : "r"(bytes.data()) : "memory");
    if (!decode_frame(bytes.data(), output))
      throw std::runtime_error("valid frame rejected");
    checksum += output.seq_num_;
  }
  const auto ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start)
          .count();
  if (checksum != 43 * count || output.me_market_update_.qty_ != 1)
    return 1;
  std::cout << "{\"operations\":" << count << ",\"ns_per_operation\":" << double(ns) / count
            << ",\"checksum\":" << checksum << "}\n";
}
