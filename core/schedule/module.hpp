#pragma once

#include <array>
#include <concepts>
#include <cstdarg>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>

#include "core/command/match.hpp"
#include "core/logging/log.hpp"

namespace daveos::core {


// Event compatibility at registration/call boundaries, after M is complete.
// This does not attempt to describe the entire module protocol.
template <typename M, typename Event>
concept ModuleFor = std::same_as<typename M::EventType, Event>;

// One named task member function. A module exposes a constexpr std::array of
// these via tasks(); names and callbacks must be nonempty/non-null and unique
// within that module. The name string must outlive the scheduler.
template <typename M>
struct TaskDescriptor {
  const char* name;
  void (M::*callback)();
};

// Parsed views borrow the dispatcher's buffer until the handler returns.
using CommandArguments = std::span<const std::string_view>;
// Command metadata is validated when constructing a dispatcher. Names/help
// strings and the module object must outlive it; use DAVEOS_COMMAND below to
// preserve the actual C++ handler identifier for log attribution.
template <typename M>
struct CommandDescriptor {
  const char* name;
  const char* help;
  Status (M::*callback)(CommandArguments);
  const char* handler;
};
// Spell the C++ function identifier once for both invocation and attribution.
#define DAVEOS_COMMAND(ModuleType, command, function, description) \
  ::daveos::core::CommandDescriptor<ModuleType> {                  \
    command, description, &ModuleType::function, #function         \
  }

template <typename Event, typename Modules, std::size_t LineCapacity = 256,
          std::size_t ArgumentCapacity = 16>
class CommandDispatcher;

// Non-owning erased reference, independent of module lists and capacities.
// Only this boundary uses indirect service calls; there are no virtual methods.
// The scheduler binds this interface before module initialization. Do not
// invoke an unbound interface or retain it beyond the owning scheduler's
// lifetime.
template <typename Event>
class SchedulerInterface {
  static_assert(std::is_enum_v<Event>);
  struct Operations {
    Status (*schedule)(void*, void*, std::size_t, Time, Mode);
    Status (*cancel)(void*, void*, std::size_t);
    Status (*post)(void*, Event, void*);
    Status (*timer)(void*, Time, TimerCallback);
    Status (*cancel_timer)(void*, TimerCallback);
    Status (*stop)(void*);
    void (*reset)(void*);
    void (*log_statistics)(void*);
#if DAVEOS_LOGGING
    Status (*log)(void*, Level, const char*, std::va_list);
#endif
    Status (*invoke)(void*, Context, Status (*)(void*), void*);
  };

 public:
  // Scheduler construction hook; application modules receive an already bound
  // view.
  template <typename Impl>
  void bind(Impl& implementation) {
    static constexpr Operations operations {
      [](void* self, void* module, std::size_t index, Time delay, Mode mode) {
        return static_cast<Impl*>(self)->ScheduleSlot(module, index, delay,
                                                      mode);
      },
          [](void* self, void* module, std::size_t index) {
            return static_cast<Impl*>(self)->CancelSlot(module, index);
          },
          [](void* self, Event event, void* sender) {
            return static_cast<Impl*>(self)->post(event, sender);
          },
          [](void* self, Time delay, TimerCallback callback) {
            return static_cast<Impl*>(self)->timer(delay, callback);
          },
          [](void* self, TimerCallback callback) {
            return static_cast<Impl*>(self)->cancel_timer(callback);
          },
          [](void* self) { return static_cast<Impl*>(self)->stop(); },
          [](void* self) { static_cast<Impl*>(self)->reset_statistics(); },
          [](void* self) { static_cast<Impl*>(self)->log_statistics(); },
#if DAVEOS_LOGGING
          [](void* self, Level level, const char* format, std::va_list args) {
            return static_cast<Impl*>(self)->LogArgs(level, format, args);
          },
#endif
          [](void* self, Context context, Status (*callback)(void*),
             void* argument) {
            return static_cast<Impl*>(self)->Invoke(context, callback,
                                                    argument);
          }
    };
    object_ = &implementation;
    operations_ = &operations;
  }
  // Schedule a registered module/task pair, replacing its pending request.
  // delay is in microseconds; repeat uses it as both initial delay and
  // interval. Zero is allowed only for once. Before run(), delays start at the
  // common run start time; while running they start at this call. Safe from
  // interrupts.
  template <typename M>
    requires ModuleFor<M, Event>
  Status schedule(M& module, void (M::*callback)(), Time delay,
                  Mode mode = Mode::once) {
    auto tasks = M::tasks();
    for (std::size_t index = 0; index < tasks.size(); ++index) {
      if (tasks[index].callback == callback)
        return operations_->schedule(object_, &module, index, delay, mode);
    }
    return Status::not_found;
  }
  // Cancel pending execution (not an already executing callback). Returns
  // not_found for an unknown/inactive task; interrupt callers are rejected.
  template <typename M>
    requires ModuleFor<M, Event>
  Status cancel(M& module, void (M::*callback)()) {
    auto tasks = M::tasks();
    for (std::size_t index = 0; index < tasks.size(); ++index) {
      if (tasks[index].callback == callback)
        return operations_->cancel(object_, &module, index);
    }
    return Status::not_found;
  }
  // Queue an enum-only broadcast, optionally excluding a registered sender.
  // Accepted before run() and from interrupts; full rejects the newest event.
  Status post(Event event, void* sender = nullptr) {
    return operations_->post(object_, event, sender);
  }
  // Interrupt-context one-shot, available only while running. Delay must be
  // positive and callback non-null. Reusing callback identity replaces its
  // timer.
  Status timer(Time delay, TimerCallback callback) {
    return operations_->timer(object_, delay, callback);
  }
  // Cancel by callback identity, including from interrupts; not_found if
  // absent.
  Status cancel_timer(TimerCallback callback) {
    return operations_->cancel_timer(object_, callback);
  }
  // Request cooperative stop if supported; returns not_running outside run().
  Status stop() { return operations_->stop(object_); }
  // Clear scheduler diagnostics/timing totals; logger counters are separate.
  void reset_statistics() { operations_->reset(object_); }
  // Queue an info-level table; normal filtering and log-buffer limits apply.
  void log_statistics() { operations_->log_statistics(object_); }
  // printf-style buffered logging, permitted from interrupts and during init.
  // Captures timestamp, context and formatted arguments now; delivers later.
  // Returns truncated for accepted shortened text, full for a dropped record.
  // Without a logger, or with logging compiled out, returns ok. Direct calls
  // still evaluate arguments; use severity macros to elide that evaluation.
#if defined(__GNUC__) || defined(__clang__)
  // The implicit this parameter counts as argument 1.
  __attribute__((format(printf, 3, 4)))
#endif
  Status
  log(Level level, const char* format, ...) {
#if DAVEOS_LOGGING
    std::va_list args;
    va_start(args, format);
    Status status = operations_->log(object_, level, format, args);
    va_end(args);
    return status;
#else
    (void)level;
    (void)format;
    return Status::ok;
#endif
  }

 private:
  template <typename, typename, std::size_t, std::size_t>
  friend class CommandDispatcher;
  // Dispatcher-only scope bridge; validates running state/interrupt context.
  Status Invoke(Context context, Status (*callback)(void*), void* argument) {
    return operations_->invoke(object_, context, callback, argument);
  }
  void* object_ = nullptr;
  const Operations* operations_ = nullptr;
};

// CRTP module defaults. Derived provides static constexpr name(); tasks(),
// commands(), command_prefix(), lifecycle, event and sleep hooks are optional.
// Callbacks run to completion on the scheduler
// thread; asynchronous work must schedule a task or maintain its own state.
// Do not move a registered module. Its name and object must outlive the
// scheduler.
template <typename Derived, typename Event>
class Module {
 public:
  using EventType = Event;
  static constexpr auto tasks() {
    return std::array<TaskDescriptor<Derived>, 0>{};
  }
  static constexpr auto commands() {
    return std::array<CommandDescriptor<Derived>, 0>{};
  }
  static constexpr const char* command_prefix() { return Derived::name(); }
  // stage1 is independent setup; stage2 may use other modules' stage1 results.
  Status init(InitStage) { return Status::ok; }
  // Broadcast reception order between modules is unspecified.
  void on_event(Event) {}
  // A veto prevents sleeping but does not prevent the scheduler from waiting.
  bool can_sleep() { return true; }
  // Valid after scheduler construction, including during both init stages.
  SchedulerInterface<Event>& scheduler() { return *scheduler_; }
  void bind(SchedulerInterface<Event>& scheduler) { scheduler_ = &scheduler; }
  // CRTP forwarding keeps callback dispatch statically typed at registration.
  Status initialize(InitStage stage) {
    return static_cast<Derived*>(this)->init(stage);
  }
  void receive(Event event) { static_cast<Derived*>(this)->on_event(event); }
  bool permits_sleep() { return static_cast<Derived*>(this)->can_sleep(); }

 protected:
  ~Module() = default;

 private:
  SchedulerInterface<Event>* scheduler_ = nullptr;
};
// Non-owning module pointers whose concrete types determine compile-time
// storage.
template <typename... Modules>
struct ModuleList {
  static_assert(UniqueNames(std::array<const char*, sizeof...(Modules)>{
                    Modules::name()...}),
                "module names must be nonempty and unique (case-insensitive)");
  std::tuple<Modules*...> items;
  explicit ModuleList(Modules*... modules) : items(modules...) {}
};
template <typename... Modules>
ModuleList(Modules*...) -> ModuleList<Modules...>;


}  // namespace daveos::core
