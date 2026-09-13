#pragma once

#include <array>
#include <cstdarg>
#include <tuple>
#include <type_traits>

#include "daveos/core/log.h"

namespace daveos::core {
template <typename M>
struct TaskDescriptor {
  const char* name;
  void (M::*callback)();
};

// Non-owning erased reference, independent of module lists and capacities.
// Only this boundary uses indirect service calls; there are no virtual methods.
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
    void (*minimum)(void*, Level);
    void (*reset)(void*);
    void (*statistics)(void*);
    Status (*log)(void*, Level, const char*, std::va_list);
  };

 public:
  template <typename Impl>
  void bind(Impl& implementation) {
    static constexpr Operations operations{
        [](void* self, void* module, std::size_t index, Time delay, Mode mode) {
          return static_cast<Impl*>(self)->schedule_slot(module, index, delay,
                                                         mode);
        },
        [](void* self, void* module, std::size_t index) {
          return static_cast<Impl*>(self)->cancel_slot(module, index);
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
        [](void* self, Level level) {
          static_cast<Impl*>(self)->minimum(level);
        },
        [](void* self) { static_cast<Impl*>(self)->reset_statistics(); },
        [](void* self) { static_cast<Impl*>(self)->log_statistics(); },
        [](void* self, Level level, const char* format, std::va_list args) {
          return static_cast<Impl*>(self)->log_args(level, format, args);
        }};
    object_ = &implementation;
    operations_ = &operations;
  }
  template <typename M>
  Status schedule(M& module, void (M::*callback)(), Time delay,
                  Mode mode = Mode::once) {
    static_assert(std::is_same_v<typename M::EventType, Event>);
    auto tasks = M::tasks();
    for (std::size_t index = 0; index < tasks.size(); ++index) {
      if (tasks[index].callback == callback)
        return operations_->schedule(object_, &module, index, delay, mode);
    }
    return Status::not_found;
  }
  template <typename M>
  Status cancel(M& module, void (M::*callback)()) {
    auto tasks = M::tasks();
    for (std::size_t index = 0; index < tasks.size(); ++index) {
      if (tasks[index].callback == callback)
        return operations_->cancel(object_, &module, index);
    }
    return Status::not_found;
  }
  Status post(Event event, void* sender = nullptr) {
    return operations_->post(object_, event, sender);
  }
  Status timer(Time delay, TimerCallback callback) {
    return operations_->timer(object_, delay, callback);
  }
  Status cancel_timer(TimerCallback callback) {
    return operations_->cancel_timer(object_, callback);
  }
  Status stop() { return operations_->stop(object_); }
  void minimum(Level level) { operations_->minimum(object_, level); }
  void reset_statistics() { operations_->reset(object_); }
  void log_statistics() { operations_->statistics(object_); }
  Status log(Level level, const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    Status status = operations_->log(object_, level, format, args);
    va_end(args);
    return status;
  }

 private:
  void* object_ = nullptr;
  const Operations* operations_ = nullptr;
};

template <typename Derived, typename Event>
class Module {
 public:
  using EventType = Event;
  explicit constexpr Module(const char* name) : name_(name) {}
  Status init(InitStage) { return Status::ok; }
  void on_event(Event) {}
  bool can_sleep() { return true; }
  const char* name() const { return name_; }
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
  const char* name_;
  SchedulerInterface<Event>* scheduler_ = nullptr;
};
template <typename... Modules>
struct ModuleList {
  std::tuple<Modules*...> items;
  explicit ModuleList(Modules*... modules) : items(modules...) {}
};
template <typename... Modules>
ModuleList(Modules*...) -> ModuleList<Modules...>;
}  // namespace daveos::core
