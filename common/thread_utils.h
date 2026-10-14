#pragma once

#include <iostream>
#include <atomic>
#include <thread>
#include <functional>
#include <memory>
#include <unistd.h>

#include <sys/syscall.h>
#include "macros.h"

namespace Common {
/// 将当前线程绑定到 core_id 指定的逻辑 CPU。
inline auto setThreadCore(int core_id) noexcept {
  cpu_set_t cpuset;

  CPU_ZERO(&cpuset);
  CPU_SET(core_id, &cpuset);

  return (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0);
}

/// 创建线程，设置绑核和线程名。
/// 在线程中调用给定函数，并转发参数。
template <typename T, typename... A>
inline auto createAndStartThread(int core_id, const std::string& name, T&& func,
                                 A&&... args) noexcept {
  auto task = std::bind(std::forward<T>(func), std::forward<A>(args)...);
  auto t = new std::thread([core_id, name, task = std::move(task)]() mutable {
    if (core_id >= 0 && !setThreadCore(core_id))
      FATAL("cannot pin thread: " + name);
    task();
  });
  return t;
}
}
