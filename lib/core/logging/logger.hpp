#pragma once

#include "core/logging/log.hpp"
#if DAVEOS_LOGGING
#include <cstdio>

#include "core/queue/queue.hpp"
#endif

namespace daveos::core {


  // Application-owned bounded log buffer. MessageSize includes the terminating
  // NUL. Producers may run in interrupt context; delivery has one scheduler
  // consumer. Formatting happens at the call site, so arguments need not
  // survive delivery. The application must choose a printf implementation
  // suitable for its ISR and allocation constraints; buffering alone does not
  // make libc formatting ISR-safe. Attach with make_scheduler(..., logger), and
  // keep this object and subscriber contexts alive through shutdown. The
  // scheduler only borrows this service.
  template <std::size_t Capacity, std::size_t MessageSize,
            std::size_t Subscribers, typename P>
  class Logger {
#if DAVEOS_LOGGING
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

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Checked by scheduler init before module callbacks. Types must match at
    // compile time; distinct instances of that type are rejected at runtime.
    bool uses_platform(const P& platform) const {
      return &platform_ == &platform;
    }

    // Filtered/no-subscriber calls return ok without queuing. full drops the
    // new record; truncated queues shortened text. Invalid formatting returns
    // an error.
    Status write(Level level, const char* format, std::va_list args) {
      if (!format) {
        return Status::invalid_argument;
      }
      Stored record;
      record.timestamp = platform_.now();
      record.context = platform_.in_interrupt() ? Context{"core", "interrupt"}
                                                : platform_.context();
      record.severity = level;
      {
        Guard guard(platform_);
        if (level < minimum_ || Subscribers == 0) {
          return Status::ok;
        }
      }
      int length =
          std::vsnprintf(record.text.data(), MessageSize, format, args);
      if (length < 0) {
        return Status::invalid_argument;
      }
      bool truncated = static_cast<std::size_t>(length) >= MessageSize;
      record.length =
          truncated ? MessageSize - 1 : static_cast<std::size_t>(length);
      Guard guard(platform_);
      if (records_.full()) {
        ++counters_.dropped;
        return Status::full;
      }
      if (truncated) {
        ++counters_.truncated;
      }
      records_.push(record);
      platform_.notify();
      return truncated ? Status::truncated : Status::ok;
    }

    // Deliver one record to all subscribers outside the lock; false means
    // empty.
    bool dispatch() {
      Stored record;
      {
        Guard guard(platform_);
        if (records_.pop(record) != Status::ok) {
          return false;
        }
      }
      LogRecord view{record.timestamp,
                     record.severity,
                     record.context.module,
                     record.context.task,
                     {record.text.data(), record.length}};
      ContextGuard context(platform_,
                           {"core", "logging", CallbackKind::logging});
      for (const auto& subscriber : subscribers_.items) {
        if (subscriber.write) {
          subscriber.write(subscriber.context, view);
        }
      }
      return true;
    }

    // Drain records during shutdown/init failure. Subscriber callbacks must
    // finish.
    void flush() {
      while (dispatch()) {
      }
    }

    // Default is info. Changing the threshold does not discard buffered
    // records.
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
#else
   public:
    Logger(P&, SubscriberList<Subscribers>) {}

    Status write(Level, const char*, std::va_list) { return Status::ok; }

    bool dispatch() { return false; }

    void flush() {}

    void minimum(Level) {}

    bool empty() { return true; }

    LogCounters counters() { return {}; }

    void reset() {}
#endif
  };

  // Deduce platform/subscriber types; sizes are records and bytes per message
  // (including terminating NUL). The disabled specialization has no storage.
  template <std::size_t Capacity = 32, std::size_t MessageSize = 128,
            typename P, std::size_t Subscribers>
  auto make_logger(P& platform, SubscriberList<Subscribers> subscribers) {
    return Logger<Capacity, MessageSize, Subscribers, P>(platform, subscribers);
  }


}  // namespace daveos::core
