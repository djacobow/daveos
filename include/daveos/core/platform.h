#pragma once

#include <cstdint>
#include <limits>

namespace daveos::core {


// Monotonic microseconds: timestamps, delays, and measured durations.
using Time = std::uint64_t;
// Sentinel for no deadline or an indefinite wait; never a real timestamp.
inline constexpr Time kForever = std::numeric_limits<Time>::max();
// Shared operation results. Failures normally leave the requested work
// unchanged; truncated is the exception: the shortened log record has been
// accepted.
enum class Status {
  ok,
  not_running,
  already_initialized,
  already_run,
  initialization_failed,
  invalid_argument,
  not_found,
  full,
  empty,
  busy,
  duplicate_name,
  truncated,
  parse_error,
  ambiguous_match,
  line_too_long,
  too_many_arguments
};
// All modules finish stage1 before any module begins stage2.
enum class InitStage { stage1, stage2 };
// Repeat cadence is based on scheduled time, not callback completion.
enum class Mode { once, repeat };
// Application timer callbacks run in interrupt context, with no payload.
using TimerCallback = void (*)();
// Borrowed names used for log attribution; strings must outlive their use.
struct Context {
  const char* module = "core";
  const char* task = "init";
};

// CRTP defaults and the contract shared by concrete platform adapters.
// Derived supplies a monotonic now(), nesting enter()/leave() critical
// sections, interrupt/context queries, and one asynchronous timer with
// arm()/disarm(). arm(delay, callback, argument) replaces the pending timer;
// even delay zero invokes the callback later in interrupt context, never inline
// in arm(). quiesce() cancels and waits for callbacks, and must be called
// outside an ISR. sequence()/notify() retain work notifications. idle(deadline,
// sleep, observed) must avoid waiting if a notification occurred after observed
// was sampled. The platform and any mutex it returns must outlive all their
// users.
template <typename Derived>
class Platform {
  struct NoMutex {
    bool try_lock() { return false; }
    void unlock() {}
  };

 public:
  using Callback = void (*)(void*);
  // A false result forces awake waiting. True still requires every module to
  // agree.
  bool can_sleep() const { return true; }
  // False makes scheduler stop requests successful no-ops while running.
  bool can_stop() const { return true; }
  // nullptr selects critical sections; otherwise provide try_lock()/unlock().
  NoMutex* queue_mutex() { return nullptr; }

 protected:
  ~Platform() = default;
};
// Scope-bound critical section. Nesting is delegated to the platform.
template <typename P>
class Guard {
 public:
  explicit Guard(P& platform) : platform_(platform) { platform_.enter(); }
  ~Guard() { platform_.leave(); }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;

 private:
  P& platform_;
};
// Temporarily attribute callbacks/logs to a module and restore the prior
// context.
template <typename P>
class ContextGuard {
 public:
  ContextGuard(P& platform, Context context)
      : platform_(platform), previous_(platform.context()) {
    platform.context(context);
  }
  ~ContextGuard() { platform_.context(previous_); }

 private:
  P& platform_;
  Context previous_;
};
// Saturating deadline addition, reserving kForever for the no-deadline
// sentinel.
constexpr Time After(Time now, Time delay) {
  return delay >= kForever - now ? kForever - 1 : now + delay;
}


}  // namespace daveos::core
