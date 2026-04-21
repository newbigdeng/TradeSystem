#pragma once

#include <string>
#include <fstream>
#include <cstdio>

#include "macros.h"
#include "lf_queue.h"
#include "thread_utils.h"
#include "time_utils.h"

namespace Common {
  /// 队列可容纳的最大元素数，8MB
  constexpr size_t LOG_QUEUE_SIZE = 8 * 1024 * 1024;

  /// 日志元素类型
  enum class LogType : int8_t {
    CHAR = 0,
    INTEGER = 1,
    LONG_INTEGER = 2,
    LONG_LONG_INTEGER = 3,
    UNSIGNED_INTEGER = 4,
    UNSIGNED_LONG_INTEGER = 5,
    UNSIGNED_LONG_LONG_INTEGER = 6,
    FLOAT = 7,
    DOUBLE = 8
  };

  /// 日志元素结构体
  struct LogElement {
    LogType type_ = LogType::CHAR;
    union { // 日志元素数据联合体
      char c;
      int i;
      long l;
      long long ll;
      unsigned u;
      unsigned long ul;
      unsigned long long ull;
      float f;
      double d;
    } u_;
  };

  class Logger final {
  public:
    /// 从日志队列中读取日志元素并写入输出日志文件
    auto flushQueue() noexcept {
      while (running_) {
        LogElement next;
        while (!queue_.try_pop(next)) ;
          switch (next.type_) {
            case LogType::CHAR:
              file_ << next.u_.c;
              break;
            case LogType::INTEGER:
              file_ << next.u_.i;
              break;
            case LogType::LONG_INTEGER:
              file_ << next.u_.l;
              break;
            case LogType::LONG_LONG_INTEGER:
              file_ << next.u_.ll;
              break;
            case LogType::UNSIGNED_INTEGER:
              file_ << next.u_.u;
              break;
            case LogType::UNSIGNED_LONG_INTEGER:
              file_ << next.u_.ul;
              break;
            case LogType::UNSIGNED_LONG_LONG_INTEGER:
              file_ << next.u_.ull;
              break;
            case LogType::FLOAT:
              file_ << next.u_.f;
              break;
            case LogType::DOUBLE:
              file_ << next.u_.d;
              break;
          }
        }
        file_.flush();

        using namespace std::literals::chrono_literals;
        std::this_thread::sleep_for(10ms);
      }
    }

    explicit Logger(const std::string &file_name)
        : file_name_(file_name), queue_(LOG_QUEUE_SIZE) {
      file_.open(file_name);
      ASSERT(file_.is_open(), "Could not open log file:" + file_name);
      logger_thread_ = createAndStartThread(-1, "Common/Logger " + file_name_, [this]() { flushQueue(); });
      ASSERT(logger_thread_ != nullptr, "Failed to start Logger thread.");
    }

    ~Logger() {
      std::string time_str;
      std::cerr << Common::getCurrentTimeStr(&time_str) << " Flushing and closing Logger for " << file_name_ << std::endl;

      while (queue_.size()) {
        using namespace std::literals::chrono_literals;
        std::this_thread::sleep_for(1s);
      }
      running_ = false;
      logger_thread_->join();

      file_.close();
      std::cerr << Common::getCurrentTimeStr(&time_str) << " Logger for " << file_name_ << " exiting." << std::endl;
    }

    /// 重载方法，将不同日志元素写入队列
    auto pushValue(const LogElement &log_element) noexcept {
      while (!queue_.try_push(log_element)) ;
    }

    auto pushValue(const char value) noexcept {
      pushValue(LogElement{LogType::CHAR, {.c = value}});
    }

    auto pushValue(const int value) noexcept {
      pushValue(LogElement{LogType::INTEGER, {.i = value}});
    }

    auto pushValue(const long value) noexcept {
      pushValue(LogElement{LogType::LONG_INTEGER, {.l = value}});
    }

    auto pushValue(const long long value) noexcept {
      pushValue(LogElement{LogType::LONG_LONG_INTEGER, {.ll = value}});
    }

    auto pushValue(const unsigned value) noexcept {
      pushValue(LogElement{LogType::UNSIGNED_INTEGER, {.u = value}});
    }

    auto pushValue(const unsigned long value) noexcept {
      pushValue(LogElement{LogType::UNSIGNED_LONG_INTEGER, {.ul = value}});
    }

    auto pushValue(const unsigned long long value) noexcept {
      pushValue(LogElement{LogType::UNSIGNED_LONG_LONG_INTEGER, {.ull = value}});
    }

    auto pushValue(const float value) noexcept {
      pushValue(LogElement{LogType::FLOAT, {.f = value}});
    }

    auto pushValue(const double value) noexcept {
      pushValue(LogElement{LogType::DOUBLE, {.d = value}});
    }

    auto pushValue(const char *value) noexcept {
      while (*value) {
        pushValue(*value);
        ++value;
      }
    }

    auto pushValue(const std::string &value) noexcept {
      pushValue(value.c_str());
    }

    /// 解析格式字符串，将变量数量替换为实际值并写入队列
    // 变参模板递归实现
    template<typename T, typename... A>
    auto log(const char *s, const T &value, A... args) noexcept {
      while (*s) {
        if (*s == '%') {
          if (UNLIKELY(*(s + 1) == '%')) { // 允许%%表示实际%
            ++s;
          } else {
            pushValue(value); // 替换%为实际值
            log(s + 1, args...); // 消去一个参数，递归处理剩余参数
            return;
          }
        }
        pushValue(*s++);
      }
      FATAL("extra arguments provided to log()");
    }

    /// 重载方法，处理字符串中没有替换的%的情况
       auto log(const char *s) noexcept {
      while (*s) {
        if (*s == '%') {
          if (UNLIKELY(*(s + 1) == '%')) { // 允许%%表示实际%
            ++s;
          } else {
            FATAL("missing arguments to log()");
          }
        }
        pushValue(*s++);
      }
    }

    /// 禁用默认构造函数、复制构造函数、移动构造函数和赋值运算符
    Logger() = delete;
    Logger(const Logger &) = delete;
    Logger(const Logger &&) = delete;
    Logger &operator=(const Logger &) = delete;
    Logger &operator=(const Logger &&) = delete;
  private:
    /// 日志文件名
    const std::string file_name_;
    std::ofstream file_;

    /// 日志队列，用于存储日志元素，并在后台线程中格式化和写入文件
    LFQueue<LogElement> queue_;
    std::atomic<bool> running_ = {true};

    /// Background logging thread.
    std::thread *logger_thread_ = nullptr;
  };
}
