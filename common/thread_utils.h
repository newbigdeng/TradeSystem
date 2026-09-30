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
  /// Set affinity for current thread to be pinned to the provided core_id.
  inline auto setThreadCore(int core_id) noexcept {
    cpu_set_t cpuset;

    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    return (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0);
  }

  /// Creates a thread instance, sets affinity on it, assigns it a name and
  /// passes the function to be run on that thread as well as the arguments to the function.
  template<typename T, typename... A>
  inline auto createAndStartThread(int core_id, const std::string &name, T &&func, A &&... args) noexcept {
    auto task=std::bind(std::forward<T>(func),std::forward<A>(args)...);
    auto t=new std::thread([core_id,name,task=std::move(task)]() mutable {
      if(core_id>=0 && !setThreadCore(core_id)) FATAL("cannot pin thread: "+name);
      task();
    });
    return t;
  }
}
