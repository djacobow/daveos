#pragma once

#include <array>
#include <cstdarg>
#include <cstdio>
#include <string_view>

#include "daveos/core/queue.h"

namespace daveos::core {
enum class Level { debug, info, warning, error, fatal };
struct LogRecord {
  Time timestamp;
  Level severity;
  const char* module;
  const char* task;
  std::string_view message;
};
struct Subscriber {
  void* context;
  void (*write)(void*, const LogRecord&);
};
template <std::size_t Size>
struct SubscriberList {
  std::array<Subscriber, Size> items;
  template <typename... Items>
  explicit SubscriberList(Items... values) : items{values...} {}
};
template <typename... Items>
SubscriberList(Items...) -> SubscriberList<sizeof...(Items)>;
struct LogCounters {
  std::uint64_t dropped = 0;
  std::uint64_t truncated = 0;
};

template <std::size_t Capacity, std::size_t MessageSize,
          std::size_t Subscribers, typename P>
class Logger {
  static_assert(MessageSize > 0);
  struct Stored {
    Time timestamp{};
    Level severity{};
    Context context{};
    std::array<char, MessageSize> text{};
    std::size_t length{};
  };

 public:
  Logger(P& platform, SubscriberList<Subscribers> subscribers)
      : platform_(platform), subscribers_(subscribers) {}
  Status write(Level level, const char* format, std::va_list args) {
    if (!format) return Status::invalid_argument;
    Stored record;
    record.timestamp = platform_.now();
    record.context = platform_.in_interrupt() ? Context{"core", "interrupt"}
                                              : platform_.context();
    record.severity = level;
    {
      Guard guard(platform_);
      if (level < minimum_ || Subscribers == 0) return Status::ok;
    }
    int length = std::vsnprintf(record.text.data(), MessageSize, format, args);
    if (length < 0) return Status::invalid_argument;
    bool truncated = static_cast<std::size_t>(length) >= MessageSize;
    record.length =
        truncated ? MessageSize - 1 : static_cast<std::size_t>(length);
    Guard guard(platform_);
    if (records_.full()) {
      ++counters_.dropped;
      return Status::full;
    }
    if (truncated) ++counters_.truncated;
    records_.push(record);
    platform_.notify();
    return truncated ? Status::truncated : Status::ok;
  }
  bool dispatch() {
    Stored record;
    {
      Guard guard(platform_);
      if (records_.pop(record) != Status::ok) return false;
    }
    LogRecord view{record.timestamp,
                   record.severity,
                   record.context.module,
                   record.context.task,
                   {record.text.data(), record.length}};
    ContextGuard context(platform_, {"core", "logging"});
    for (const auto& subscriber : subscribers_.items) {
      if (subscriber.write) subscriber.write(subscriber.context, view);
    }
    return true;
  }
  void flush() {
    while (dispatch()) {
    }
  }
  void minimum(Level level) {
    Guard guard(platform_);
    minimum_ = level;
  }
  bool empty() {
    Guard guard(platform_);
    return records_.empty();
  }
  LogCounters counters() {
    Guard guard(platform_);
    return counters_;
  }
  void reset() {
    Guard guard(platform_);
    counters_ = {};
  }

 private:
  P& platform_;
  SubscriberList<Subscribers> subscribers_;
  Queue<Stored, Capacity> records_;
  Level minimum_ = Level::info;
  LogCounters counters_{};
};
}  // namespace daveos::core
