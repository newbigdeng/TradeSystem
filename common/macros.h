#pragma once

#include <cstring>
#include <iostream>
#include <string_view>

/// 分支预测提示。
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)

// 条件仍会求值且失败仍退出。string_view 避免成功路径为字面量构造临时 string。
// 动态拼接的错误文本依然先求值，不能据此认为所有 ASSERT 都零分配。
inline auto ASSERT(bool cond, std::string_view msg) noexcept {
  if (UNLIKELY(!cond)) {
    std::cerr << "ASSERT : " << msg << std::endl;

    exit(EXIT_FAILURE);
  }
}

inline auto FATAL(const std::string& msg) noexcept {
  std::cerr << "FATAL : " << msg << std::endl;

  exit(EXIT_FAILURE);
}
