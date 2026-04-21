#pragma once

#include <iostream>
#include <atomic>
#include <thread>
#include <unistd.h>

#include <sys/syscall.h>

namespace Common {
  /// 设置当前线程的亲和性，将其固定到提供的 core_id 上。
  inline auto setThreadCore(int core_id) noexcept {
    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    return (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0);
  }

  /// 创建一个线程实例，设置其亲和性，为其分配一个名称并
  /// 传递要在该线程上运行的函数以及函数参数。
  template<typename T, typename... A>
  inline auto createAndStartThread(int core_id, const std::string &name, T &&func, A &&... args) noexcept {
    auto t = new std::thread([&]() {
      if (core_id >= 0 && !setThreadCore(core_id)) {
        std::cerr << "Failed to set core affinity for " << name << " " << pthread_self() << " to " << core_id << std::endl;
        exit(EXIT_FAILURE);
      }
      std::cerr << "Set core affinity for " << name << " " << pthread_self() << " to " << core_id << std::endl;

      std::forward<T>(func)((std::forward<A>(args))...);
    });

    using namespace std::literals::chrono_literals;
    std::this_thread::sleep_for(1s);

    return t;
  }
}