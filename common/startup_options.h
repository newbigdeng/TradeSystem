#pragma once

#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include "runtime_config.h"

namespace Common {
struct StartupOptions {
  std::string config_path;
  int first_positional = 1;
  bool check_only = false;
};

// 默认配置随可执行文件放在 config/，启动目录改变也不会读错文件。
inline StartupOptions startupOptions(int argc, char** argv, const char* filename) {
  StartupOptions result;
  for (int i = 1; i < argc; ++i) {
    const std::string_view option(argv[i]);
    if (option == "--config") {
      if (!result.config_path.empty() || i + 1 >= argc || std::string_view(argv[i + 1]).empty())
        throw ConfigError("--config 需要且只能指定一个文件");
      result.config_path = argv[++i];
    } else if (option == "--check-config") {
      result.check_only = true;
    } else {
      result.first_positional = i;
      break;
    }
    result.first_positional = i + 1;
  }
  if (result.config_path.empty())
    result.config_path =
        (std::filesystem::canonical("/proc/self/exe").parent_path() / "config" / filename).string();
  return result;
}

inline uint64_t unsignedSetting(std::string_view text, uint64_t maximum, const std::string& field) {
  uint64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value > maximum)
    throw ConfigError(field + " 必须是范围内的非负整数");
  return value;
}

inline double floatingSetting(std::string_view text, const std::string& field) {
  double value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(value))
    throw ConfigError(field + " 必须是有限数值");
  return value;
}

// 保留测量脚本的环境变量入口；环境变量优先于 JSON，仍在启动前校验。
inline uint64_t environmentSetting(const char* key, uint64_t fallback, uint64_t maximum) {
  const auto* text = std::getenv(key);
  return text ? unsignedSetting(text, maximum, key) : fallback;
}
}
