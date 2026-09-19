#pragma once

#include <array>
#include <cstring>
#include <type_traits>
#include <utility>

#include "core/logging/log_format.hpp"
#include "core/queue/queue.hpp"
#include "core/schedule/module.hpp"

namespace daveos::core {


// Completed task iterations since reset. Durations/lateness are microseconds;
// min/max/average are zero before the first completion. Any late start counts.
// Name pointers are borrowed from the registered module/task descriptors.
struct TaskStatistics {
  const char* module = "";
  const char* task = "";
  std::uint64_t executions = 0;
  std::uint64_t late_starts = 0;
  Time max_lateness = 0;
  Time min_duration = 0;
  Time max_duration = 0;
  Time total_duration = 0;

  double average() const {
    return executions ? static_cast<double>(total_duration) /
                            static_cast<double>(executions)
                      : 0;
  }
};

// Copyable diagnostic snapshot; task entries remain in registration order.
template <std::size_t Tasks>
struct Statistics {
  std::array<TaskStatistics, Tasks> tasks;
  std::uint64_t event_overflows = 0;
  std::uint64_t timer_overflows = 0;
};

// Fixed-storage cooperative scheduler. Prefer make_scheduler() for deduction.
// Event is the application's enum. ModuleList determines module/task capacity.
// Logging is an optional borrowed service; the scheduler owns no log buffers.
// Platform, modules, logger and name strings must outlive this
// object. At least one module is required; task arrays may be empty.
// One thread owns init()/run()/destruction; these are never ISR entry points.
template <typename Event, typename Modules, typename Logging, typename P,
          std::size_t EventCapacity = 32, std::size_t TimerCapacity = 16>
class Scheduler;

template <typename Event, typename P, typename... Modules, typename Logging,
          std::size_t EventCapacity, std::size_t TimerCapacity>
class Scheduler<Event, ModuleList<Modules...>, Logging, P, EventCapacity,
                TimerCapacity>
    final : public SchedulerInterface<Event> {
  static constexpr bool kHasLogging = !std::is_same_v<Logging, NoLogging>;
  static constexpr std::size_t kTasks =
      (std::tuple_size_v<decltype(Modules::tasks())> + ... + 0);
  static_assert(sizeof...(Modules) > 0);
  enum class State {
    fresh,
    initializing,
    ready,
    running,
    stopping,
    stopped,
    failed
  };

  struct Registration {
    void* object = nullptr;
    const char* name = "";
    void (*bind)(void*, SchedulerInterface<Event>&) = nullptr;
    Status (*init)(void*, InitStage) = nullptr;
    void (*event)(void*, Event) = nullptr;
    bool (*sleep)(void*) = nullptr;
  };

  struct Task {
    void* module = nullptr;
    const char* module_name = "";
    void* object = nullptr;
    void (*invoke)(void*, std::size_t) = nullptr;
    std::size_t index = 0;
    const char* name = "";
    bool active = false;
    Mode mode = Mode::once;
    Time interval = 0;
    Time due = 0;
  };

  struct EventRecord {
    Event value{};
    void* sender{};
    Time due{};
  };

  struct Timer {
    bool active = false;
    Time due = 0;
    TimerCallback callback = nullptr;
  };

 public:
  // Store references and static descriptors only; binding and callbacks wait
  // for init(), after all application objects have been constructed.
  Scheduler(P& platform, ModuleList<Modules...> modules, Logging logging = {})
      : platform_(platform), logging_(logging) {
    this->bind(*this);
    std::apply([&](auto*... module) { (Register(module), ...); },
               modules.items);
  }

  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;

  // Quiesce platform callbacks before releasing scheduler-owned storage.
  ~Scheduler() { platform_.quiesce(); }

  // Validate registration, then run stage1 for every module followed by stage2.
  // First failure is terminal: discard queued tasks/events and flush logs.
  // Successful initialization is never repeated; later calls report an error.
  Status init() {
    {
      Guard guard(platform_);
      if (state_ == State::failed) return failure_;
      if (state_ != State::fresh) return Status::already_initialized;
      state_ = State::initializing;
    }
    Status status = Validate();
    if (status == Status::ok) {
      for (const auto& module : modules_) module.bind(module.object, *this);
      for (auto stage : {InitStage::stage1, InitStage::stage2}) {
        for (const auto& module : modules_) {
          ContextGuard context(platform_, {module.name, "init"});
          status = module.init(module.object, stage);
          if (status != Status::ok) break;
        }
        if (status != Status::ok) break;
      }
    }
    {
      Guard guard(platform_);
      if (status == Status::ok) {
        state_ = State::ready;
      } else {
        state_ = State::failed;
        failure_ = status;
        for (auto& task : tasks_) task.active = false;
        event_count_ = 0;
      }
    }
    if constexpr (kHasLogging)
      if (status != Status::ok) logging_.flush();
    return status;
  }

  // Single-use dispatch loop; initialize automatically when still fresh.
  // Due tasks/events run earliest-first, with unspecified tie order. Repeating
  // tasks retain every overdue iteration and advance from their scheduled
  // times. Logs dispatch one record at a time when no task/event is due. Idle
  // considers both task and timer deadlines and sleeps only when every module
  // permits it. A supported stop waits for active callbacks, discards work,
  // then flushes logs.
  Status run() {
    bool initialize = false;
    {
      Guard guard(platform_);
      if (used_) return Status::already_run;
      used_ = true;
      if (state_ == State::failed) return failure_;
      if (state_ != State::fresh && state_ != State::ready) return Status::busy;
      initialize = state_ == State::fresh;
    }
    if (initialize) {
      Status status = init();
      if (status != Status::ok) return status;
    }
    {
      Guard guard(platform_);
      state_ = State::running;
      Time start = platform_.now();
      for (auto& task : tasks_)
        if (task.active) task.due = After(start, task.interval);
    }
    while (true) {
      auto observed = platform_.sequence();
      std::size_t task_index = kTasks;
      EventRecord event;
      bool has_event = false;
      Time scheduled = 0;
      {
        Guard guard(platform_);
        if (stop_requested_) break;
        Time now = platform_.now();
        Time earliest = kForever;
        for (std::size_t index = 0; index < kTasks; ++index) {
          if (tasks_[index].active && tasks_[index].due <= now &&
              tasks_[index].due < earliest) {
            earliest = tasks_[index].due;
            task_index = index;
          }
        }
        std::size_t event_index = event_count_;
        for (std::size_t index = 0; index < event_count_; ++index) {
          if (events_[index].due <= now && events_[index].due < earliest) {
            earliest = events_[index].due;
            event_index = index;
          }
        }
        if (event_index < event_count_) {
          event = events_[event_index];
          events_[event_index] = events_[--event_count_];
          has_event = true;
          task_index = kTasks;
        } else if (task_index < kTasks) {
          auto& task = tasks_[task_index];
          scheduled = task.due;
          // Advance before invocation: explicit callback/ISR changes always
          // win.
          if (task.mode == Mode::repeat)
            task.due = After(task.due, task.interval);
          else
            task.active = false;
        }
      }
      if (has_event) {
        for (const auto& module : modules_) {
          if (module.object == event.sender) continue;
          ContextGuard context(platform_, {module.name, "event"});
          module.event(module.object, event.value);
        }
        continue;
      }
      if (task_index < kTasks) {
        auto& task = tasks_[task_index];
        Time start = platform_.now();
        {
          ContextGuard context(platform_, {task.module_name, task.name});
          task.invoke(task.object, task.index);
        }
        Time duration = platform_.now() - start;
        Guard guard(platform_);
        auto& stats = statistics_.tasks[task_index];
        if (!stats.executions || duration < stats.min_duration)
          stats.min_duration = duration;
        if (duration > stats.max_duration) stats.max_duration = duration;
        ++stats.executions;
        stats.total_duration += duration;
        if (start > scheduled) {
          ++stats.late_starts;
          if (start - scheduled > stats.max_lateness)
            stats.max_lateness = start - scheduled;
        }
        continue;
      }
      if constexpr (kHasLogging)
        if (logging_.dispatch()) continue;
      bool sleep = platform_.can_sleep();
      for (const auto& module : modules_) {
        ContextGuard context(platform_, {module.name, "can_sleep"});
        if (!module.sleep(module.object)) sleep = false;
      }
      Time deadline = kForever;
      {
        Guard guard(platform_);
        if (stop_requested_) continue;
        if constexpr (kHasLogging)
          if (!logging_.empty()) continue;
        for (const auto& task : tasks_)
          if (task.active && task.due < deadline) deadline = task.due;
        timers_[TimerCapacity] = {deadline != kForever, deadline, nullptr};
        for (const auto& timer : timers_)
          if (timer.active && timer.due < deadline) deadline = timer.due;
        Rearm();
      }
      platform_.idle(deadline, sleep, observed);
    }
    {
      Guard guard(platform_);
      state_ = State::stopping;
      for (auto& timer : timers_) timer.active = false;
      platform_.disarm();
      for (auto& task : tasks_) task.active = false;
      event_count_ = 0;
    }
    platform_.quiesce();
    if constexpr (kHasLogging) logging_.flush();
    {
      Guard guard(platform_);
      state_ = State::stopped;
    }
    return Status::ok;
  }

  // ISR-safe cooperative request; does not interrupt the current callback or
  // partially delivered broadcast. Platforms may make this a successful no-op.
  Status stop() {
    Guard guard(platform_);
    if (state_ != State::running) return Status::not_running;
    if (platform_.can_stop()) {
      stop_requested_ = true;
      platform_.notify();
    }
    return Status::ok;
  }

  // Queue a timestamped broadcast. sender, if supplied, must be registered and
  // is excluded from reception. Queue overflow increments event_overflows.
  Status post(Event value, void* sender = nullptr) {
    Guard guard(platform_);
    if (!AcceptsWork()) return Status::not_running;
    if (sender && !Contains(sender)) return Status::not_found;
    if (event_count_ == EventCapacity) {
      ++statistics_.event_overflows;
      return Status::full;
    }
    events_[event_count_++] = {value, sender, platform_.now()};
    platform_.notify();
    return Status::ok;
  }

  // Multiplex a positive-delay callback over the one platform timer. Safe from
  // interrupts, but rejected before run(). Replacing a callback needs no new
  // slot; a new callback at capacity fails and increments timer_overflows.
  Status timer(Time delay, TimerCallback callback) {
    Guard guard(platform_);
    if (state_ != State::running) return Status::not_running;
    if (!delay || !callback) return Status::invalid_argument;
    std::size_t slot = TimerCapacity;
    for (std::size_t index = 0; index < TimerCapacity; ++index) {
      if (timers_[index].active && timers_[index].callback == callback) {
        slot = index;
        break;
      }
      if (!timers_[index].active) slot = index;
    }
    if (slot == TimerCapacity) {
      ++statistics_.timer_overflows;
      return Status::full;
    }
    timers_[slot] = {true, After(platform_.now(), delay), callback};
    Rearm();
    platform_.notify();
    return Status::ok;
  }

  // ISR-safe cancellation of a pending callback, identified by function
  // pointer.
  Status cancel_timer(TimerCallback callback) {
    Guard guard(platform_);
    if (state_ != State::running) return Status::not_running;
    for (std::size_t index = 0; index < TimerCapacity; ++index) {
      if (timers_[index].active && timers_[index].callback == callback) {
        timers_[index].active = false;
        Rearm();
        platform_.notify();
        return Status::ok;
      }
    }
    return Status::not_found;
  }

  // Synchronized copy, available in every lifecycle state, including from ISRs.
  // Callers own the values, but module/task name strings remain borrowed.
  Statistics<kTasks> snapshot() {
    Guard guard(platform_);
    return statistics_;
  }

  // Reset timing/counters while retaining names, pending work and buffered
  // logs. An in-progress task records its whole iteration when it subsequently
  // finishes.
  void reset_statistics() {
    Guard guard(platform_);
    statistics_ = {};
    for (std::size_t index = 0; index < kTasks; ++index) {
      statistics_.tasks[index].module = tasks_[index].module_name;
      statistics_.tasks[index].task = tasks_[index].name;
    }
  }

  // Queue an info-level snapshot table; it may be filtered, truncated or
  // dropped.
  void log_statistics() {
#if DAVEOS_LOGGING
    auto stats = snapshot();
    this->log(Level::info,
              "Module/task | calls late | lateness min max avg (us)");
    for (const auto& task : stats.tasks) {
      this->log(Level::info, "%s/%s | %s %s | %s %s %s %.1f", task.module,
                task.task, LogUnsigned(task.executions).c_str(),
                LogUnsigned(task.late_starts).c_str(),
                LogUnsigned(task.max_lateness).c_str(),
                LogUnsigned(task.min_duration).c_str(),
                LogUnsigned(task.max_duration).c_str(), task.average());
    }
    this->log(Level::info, "Overflow events=%s timers=%s",
              LogUnsigned(stats.event_overflows).c_str(),
              LogUnsigned(stats.timer_overflows).c_str());
#endif
  }

 private:
  friend class SchedulerInterface<Event>;

  bool AcceptsWork() const {
    return state_ == State::fresh || state_ == State::initializing ||
           state_ == State::ready || state_ == State::running;
  }

  bool Contains(void* module) const {
    for (const auto& entry : modules_)
      if (entry.object == module) return true;
    return false;
  }

  template <typename M>
    requires ModuleFor<M, Event>
  void Register(M* module) {
    if (!module) {
      registration_error_ = true;
      return;
    }
    modules_[module_count_++] = {
        module,
        M::name(),
        [](void* self, SchedulerInterface<Event>& scheduler) {
          static_cast<M*>(self)->bind(scheduler);
        },
        [](void* self, InitStage stage) {
          return static_cast<M*>(self)->initialize(stage);
        },
        [](void* self, Event event) { static_cast<M*>(self)->receive(event); },
        [](void* self) { return static_cast<M*>(self)->permits_sleep(); }};
    constexpr auto descriptors = M::tasks();
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
      tasks_[task_count_] = {
          module,
          M::name(),
          module,
          [](void* object, std::size_t slot) {
            constexpr auto tasks = M::tasks();
            (static_cast<M*>(object)->*tasks[slot].callback)();
          },
          index,
          descriptors[index].name};
      statistics_.tasks[task_count_++] = {M::name(), descriptors[index].name};
      if (!descriptors[index].callback) registration_error_ = true;
      for (std::size_t previous = 0; previous < index; ++previous) {
        if (descriptors[previous].callback == descriptors[index].callback)
          registration_error_ = true;
      }
    }
  }

  Status Validate() {
    if constexpr (kHasLogging)
      if (!logging_.uses_platform(platform_)) return Status::invalid_argument;
    if (registration_error_) return Status::invalid_argument;
    for (std::size_t index = 0; index < kTasks; ++index) {
      if (!tasks_[index].name || !*tasks_[index].name)
        return Status::invalid_argument;
      for (std::size_t previous = 0; previous < index; ++previous) {
        if (tasks_[index].module == tasks_[previous].module &&
            std::strcmp(tasks_[index].name, tasks_[previous].name) == 0)
          return Status::duplicate_name;
      }
    }
    return Status::ok;
  }

  Status ScheduleSlot(void* module, std::size_t index, Time delay, Mode mode) {
    Guard guard(platform_);
    if (!AcceptsWork()) return Status::not_running;
    if (mode == Mode::repeat && !delay) return Status::invalid_argument;
    for (auto& task : tasks_)
      if (task.module == module && task.index == index) {
        task.active = true;
        task.interval = delay;
        task.mode = mode;
        task.due =
            state_ == State::running ? After(platform_.now(), delay) : delay;
        platform_.notify();
        return Status::ok;
      }
    return Status::not_found;
  }

  Status CancelSlot(void* module, std::size_t index) {
    if (platform_.in_interrupt()) return Status::invalid_argument;
    Guard guard(platform_);
    if (!AcceptsWork()) return Status::not_running;
    for (auto& task : tasks_)
      if (task.module == module && task.index == index) {
        if (!task.active) return Status::not_found;
        task.active = false;
        platform_.notify();
        return Status::ok;
      }
    return Status::not_found;
  }

  Status Invoke(Context context, Status (*callback)(void*), void* argument) {
    if (platform_.in_interrupt()) return Status::invalid_argument;
    {
      Guard guard(platform_);
      if (state_ != State::running) return Status::not_running;
    }
    ContextGuard guard(platform_, context);
    return callback(argument);
  }

  Status LogArgs(Level level, const char* format, std::va_list args) {
    return logging_.write(level, format, args);
  }

  // Called under the platform guard; the final slot is the scheduler's own wake
  // timer.
  void Rearm() {
    Time earliest = kForever;
    for (const auto& timer : timers_)
      if (timer.active && timer.due < earliest) earliest = timer.due;
    if (earliest == kForever) {
      platform_.disarm();
      return;
    }
    Time now = platform_.now();
    platform_.arm(
        earliest > now ? earliest - now : 0,
        [](void* self) { static_cast<Scheduler*>(self)->Fire(); }, this);
  }

  // Platform interrupt callback: remove each due timer before invoking it so
  // callbacks can safely rearm/cancel timers. User code runs outside our guard.
  void Fire() {
    while (true) {
      TimerCallback callback = nullptr;
      {
        Guard guard(platform_);
        if (state_ != State::running) return;
        std::size_t slot = timers_.size();
        Time earliest = kForever;
        Time now = platform_.now();
        for (std::size_t index = 0; index < timers_.size(); ++index) {
          if (timers_[index].active && timers_[index].due <= now &&
              timers_[index].due < earliest) {
            earliest = timers_[index].due;
            slot = index;
          }
        }
        if (slot == timers_.size()) {
          Rearm();
          break;
        }
        callback = timers_[slot].callback;
        timers_[slot].active = false;
      }
      if (callback) callback();
      platform_.notify();
    }
  }

  P& platform_;
  [[no_unique_address]] Logging logging_;
  std::array<Registration, sizeof...(Modules)> modules_{};
  std::array<Task, kTasks> tasks_{};
  std::array<EventRecord, EventCapacity> events_{};
  std::array<Timer, TimerCapacity + 1> timers_{};
  Statistics<kTasks> statistics_{};
  std::size_t module_count_ = 0, task_count_ = 0, event_count_ = 0;
  bool registration_error_ = false, used_ = false, stop_requested_ = false;
  State state_ = State::fresh;
  Status failure_ = Status::initialization_failed;
};

// Deduce module/platform types; optional sizes are event and timer slots.
// With no logger there is no logging storage or work. An attached logger is
// borrowed and must use the same platform and outlive the scheduler.
template <typename Event, std::size_t Events = 32, std::size_t Timers = 16,
          typename P, typename... Modules>
auto make_scheduler(P& platform, ModuleList<Modules...> modules) {
  return Scheduler<Event, ModuleList<Modules...>, NoLogging, P, Events, Timers>(
      platform, modules);
}

template <typename Event, std::size_t Events = 32, std::size_t Timers = 16,
          typename P, typename... Modules, typename L>
auto make_scheduler(P& platform, ModuleList<Modules...> modules, L& logger) {
#if DAVEOS_LOGGING
  return Scheduler<Event, ModuleList<Modules...>, LogService<L>, P, Events,
                   Timers>(platform, modules, LogService<L>(logger));
#else
  (void)logger;
  return make_scheduler<Event, Events, Timers>(platform, modules);
#endif
}


}  // namespace daveos::core
