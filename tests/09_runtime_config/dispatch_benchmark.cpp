#include "dispatch_benchmark.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 2)
    return 2;
  const std::string mode = argv[1];
  if (mode != "function" && mode != "virtual")
    return 2;

  Audit::Payload payload;
  auto callbacks = Audit::callbacks(payload);
  auto strategy = Audit::strategy(payload);
  Exchange::MEMarketUpdate event;
  event.qty_ = 5;
  event.order_id_ = 7;
  Exchange::MEClientResponse response;
  response.leaves_qty_ = 3;
  response.response_id_ = 9;
  constexpr size_t per_sample = 100000;
  constexpr size_t samples = 100;
  constexpr size_t warmups = 10;
  std::vector<double> elapsed;
  elapsed.reserve(samples);

  for (size_t sample = 0; sample < warmups + samples; ++sample) {
    const auto begin = std::chrono::steady_clock::now();
    if (mode == "function") {
      for (size_t i = 0; i < per_sample; ++i) {
        callbacks.book(0, 100, Common::Side::BUY, nullptr);
        callbacks.trade(&event, nullptr);
        callbacks.order(&response);
      }
    } else {
      for (size_t i = 0; i < per_sample; ++i) {
        strategy->book(0, 100, Common::Side::BUY, nullptr);
        strategy->trade(&event, nullptr);
        strategy->order(&response);
      }
    }
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - begin)
                        .count();
    if (sample >= warmups)
      elapsed.push_back(double(ns) / (per_sample * 3));
  }
  const auto expected = uint64_t(per_sample) * (warmups + samples) * (101 + 12 + 12);
  if (payload.total != expected)
    throw std::runtime_error("callback arithmetic differs");
  std::sort(elapsed.begin(), elapsed.end());
  std::cout << "{\"mode\":\"" << mode << "\",\"calls\":" << per_sample * samples * 3
            << ",\"p50_ns\":" << elapsed[50] << ",\"p99_ns\":" << elapsed[99]
            << ",\"total\":" << payload.total << "}\n";
}
