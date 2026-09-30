#pragma once
#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include "macros.h"
#include "lf_queue.h"
#include "thread_utils.h"
#include "time_utils.h"
namespace Common {
constexpr size_t LOG_QUEUE_SIZE=8192;
// Diagnostic logger: whole records are accepted or dropped with an explicit
// counter. This is not a durable order journal. Multiple writers are serialized.
class Logger final {
 public:
  explicit Logger(const std::string &name,size_t capacity=LOG_QUEUE_SIZE):queue_(capacity),file_(name) {
    ASSERT(file_.is_open(),"cannot open log file: "+name);
    worker_=std::thread([this] {
      std::string record;
      while(running_.load(std::memory_order_acquire) || queue_.peek()) {
        bool written=false;
        while(queue_.try_pop(record)) { file_<<record; written=true; }
        if(written) file_.flush();
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      file_<<"[LOGGER] dropped_records="<<dropped_.load()<<'\n'; file_.flush();
    });
  }
  ~Logger() { running_.store(false,std::memory_order_release); worker_.join(); }
  template<class... Args> void log(const char *format,const Args&... args) noexcept {
    std::ostringstream text; formatTo(text,format,args...);
    const auto record=text.str();
    std::lock_guard<std::mutex> lock(producers_);
    if(record.size()>65536 || !queue_.try_push(record)) dropped_.fetch_add(1,std::memory_order_relaxed);
  }
  uint64_t dropped_records() const noexcept { return dropped_.load(std::memory_order_relaxed); }
  Logger(const Logger &)=delete; Logger &operator=(const Logger &)=delete;
 private:
  static void formatTo(std::ostringstream &out,const char *format) {
    while(*format) {
      if(*format=='%') { ASSERT(format[1]=='%',"missing logger arguments"); ++format; }
      out<<*format++;
    }
  }
  template<class T,class... Args> static void formatTo(std::ostringstream &out,const char *format,const T &value,const Args&... args) {
    while(*format) {
      if(*format=='%') {
        if(format[1]=='%') ++format;
        else { out<<value; formatTo(out,format+1,args...); return; }
      }
      out<<*format++;
    }
    FATAL("extra logger arguments");
  }
  LFQueue<std::string> queue_; std::ofstream file_; std::mutex producers_;
  std::atomic<bool> running_{true}; std::atomic<uint64_t> dropped_{0}; std::thread worker_;
};
}
