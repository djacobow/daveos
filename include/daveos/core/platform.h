#pragma once

#include <cstdint>
#include <limits>

namespace daveos::core {
using Time = std::uint64_t;
inline constexpr Time kForever = std::numeric_limits<Time>::max();
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
  truncated
};
enum class InitStage { stage1, stage2 };
enum class Mode { once, repeat };
using TimerCallback = void (*)();
struct Context {
  const char* module = "core";
  const char* task = "init";
};

// Defaults and common callback vocabulary for statically dispatched platforms.
// Derived supplies now, enter/leave, arm/disarm/quiesce, context, idle and
// notification.
template <typename Derived>
class Platform {
  struct NoMutex {
    bool try_lock() { return false; }
    void unlock() {}
  };

 public:
  using Callback = void (*)(void*);
  bool can_sleep() const { return true; }
  bool can_stop() const { return true; }
  NoMutex* queue_mutex() { return nullptr; }
  Derived& implementation() { return static_cast<Derived&>(*this); }

 protected:
  ~Platform() = default;
};
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
constexpr Time After(Time now, Time delay) {
  return delay >= kForever - now ? kForever - 1 : now + delay;
}
}  // namespace daveos::core
