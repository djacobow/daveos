#pragma once

#include <array>
#include <cstdarg>
#include <string_view>

#include "daveos/core/platform.h"

// Non-Meson consumers default to logging enabled. This setting must agree
// across every translation unit in an application.
#ifndef DAVEOS_LOGGING
#define DAVEOS_LOGGING 1
#endif

// General shorthand and module severity aliases. Disabled macros evaluate
// neither the target nor any arguments; their result remains Status::ok.
// Enabled macros preserve printf checking and evaluate arguments once, even
// when filtered at runtime. F_ sets severity only; it never halts.
#if DAVEOS_LOGGING
#define DAVEOS_LOG(target, level, ...) ((target).log(level, __VA_ARGS__))
#else
#define DAVEOS_LOG(target, level, ...) (::daveos::core::Status::ok)
#endif
#define D_(...) \
  DAVEOS_LOG(this->scheduler(), ::daveos::core::Level::debug, __VA_ARGS__)
#define I_(...) \
  DAVEOS_LOG(this->scheduler(), ::daveos::core::Level::info, __VA_ARGS__)
#define W_(...) \
  DAVEOS_LOG(this->scheduler(), ::daveos::core::Level::warning, __VA_ARGS__)
#define E_(...) \
  DAVEOS_LOG(this->scheduler(), ::daveos::core::Level::error, __VA_ARGS__)
#define F_(...) \
  DAVEOS_LOG(this->scheduler(), ::daveos::core::Level::fatal, __VA_ARGS__)

namespace daveos::core {


// Ordered severity threshold; fatal has no special control-flow behavior.
enum class Level { debug, info, warning, error, fatal };
// Subscriber view of a buffered record. Timestamp is captured at the log call;
// message storage is valid only for the duration of the subscriber callback.
// Names are borrowed from registration or platform context. Outputters supply
// the line ending; callers need not include one in the format string.
struct LogRecord {
  Time timestamp;
  Level severity;
  const char* module;
  const char* task;
  std::string_view message;
};
// Non-owning output callback/context pair. The context must outlive the
// logger. Delivery occurs in scheduler context, never synchronously from the
// log caller.
struct Subscriber {
  void* context;
  void (*write)(void*, const LogRecord&);
};
// Constructor arguments determine subscriber storage at compile time.
template <std::size_t Size>
struct SubscriberList {
  std::array<Subscriber, Size> items;
  template <typename... Items>
  explicit SubscriberList(Items... values) : items{values...} {}
};
template <typename... Items>
SubscriberList(Items...) -> SubscriberList<sizeof...(Items)>;
// Cumulative counts until reset: rejected full-buffer writes and stored
// truncations.
struct LogCounters {
  std::uint64_t dropped = 0;
  std::uint64_t truncated = 0;
};

// Empty scheduler policy: no storage, formatting, idle work or shutdown work.
struct NoLogging {
  static Status write(Level, const char*, std::va_list) { return Status::ok; }
  static bool dispatch() { return false; }
  static void flush() {}
  static bool empty() { return true; }
};
// Statically typed, non-owning attachment. The logger and scheduler must use
// the same platform; the logger must outlive scheduler shutdown/destruction.
template <typename L>
class LogService {
 public:
  explicit LogService(L& logger) : logger_(logger) {}
  template <typename P>
  bool uses_platform(const P& platform) const {
    return logger_.uses_platform(platform);
  }
  Status write(Level level, const char* format, std::va_list args) {
    return logger_.write(level, format, args);
  }
  bool dispatch() { return logger_.dispatch(); }
  void flush() { logger_.flush(); }
  bool empty() { return logger_.empty(); }

 private:
  L& logger_;
};


}  // namespace daveos::core
