#pragma once

#include <array>
#include <cstdarg>
#include <cstdio>
#include <string_view>

#include "daveos/core/queue.h"

// Module member-function shorthand. Each expression returns the log Status.
// Forward the format string and any arguments unchanged for compiler checking.
// Example: I_("ready") or E_("error %d", code). Arguments are evaluated once,
// including for filtered messages. F_ sets severity only; it does not halt.
#define D_(...) \
  (this->scheduler().log(::daveos::core::Level::debug, __VA_ARGS__))
#define I_(...) \
  (this->scheduler().log(::daveos::core::Level::info, __VA_ARGS__))
#define W_(...) \
  (this->scheduler().log(::daveos::core::Level::warning, __VA_ARGS__))
#define E_(...) \
  (this->scheduler().log(::daveos::core::Level::error, __VA_ARGS__))
#define F_(...) \
  (this->scheduler().log(::daveos::core::Level::fatal, __VA_ARGS__))

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
// scheduler. Delivery occurs in scheduler context, never synchronously from the
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

// Scheduler-owned bounded log buffer. MessageSize includes the terminating NUL.
// Producers may run in interrupt context; delivery has one scheduler consumer.
// Formatting happens at the call site, so arguments need not survive delivery.
// The application must choose a printf implementation suitable for its ISR and
// allocation constraints; buffering alone does not make libc formatting
// ISR-safe.
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
  // Filtered/no-subscriber calls return ok without queuing. full drops the new
  // record; truncated queues shortened text. Invalid formatting returns an
  // error.
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
  // Deliver one record to all subscribers outside the lock; false means empty.
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
  // Drain records during shutdown/init failure. Subscriber callbacks must
  // finish.
  void flush() {
    while (dispatch()) {
    }
  }
  // Default is info. Changing the threshold does not discard buffered records.
  void minimum(Level level) {
    Guard guard(platform_);
    minimum_ = level;
  }
  bool empty() {
    Guard guard(platform_);
    return records_.empty();
  }
  // Return a synchronized copy; the caller owns the result.
  LogCounters counters() {
    Guard guard(platform_);
    return counters_;
  }
  // Reset diagnostic counts only; retain queued records and the level
  // threshold.
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
