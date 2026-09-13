#pragma once

#include <array>
#include <cstring>
#include <utility>

#include "daveos/core/module.h"

namespace daveos::core {
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
template <std::size_t Tasks>
struct Statistics {
  std::array<TaskStatistics, Tasks> tasks;
  std::uint64_t event_overflows = 0;
  std::uint64_t timer_overflows = 0;
  LogCounters logs;
};

template <typename Event, typename Modules, typename Subscribers, typename P,
          std::size_t EventCapacity = 32, std::size_t TimerCapacity = 16,
          std::size_t LogCapacity = 32, std::size_t MessageSize = 128>
class Scheduler;

template <typename Event, typename P, typename... Modules,
          std::size_t Subscribers, std::size_t EventCapacity,
          std::size_t TimerCapacity, std::size_t LogCapacity,
          std::size_t MessageSize>
class Scheduler<Event, ModuleList<Modules...>, SubscriberList<Subscribers>, P,
                EventCapacity, TimerCapacity, LogCapacity, MessageSize>
    final : public SchedulerInterface<Event> {
  static constexpr std::size_t kTasks =
      (std::tuple_size_v<decltype(Modules::tasks())> + ... + 0);
  static_assert(sizeof...(Modules) > 0 && kTasks > 0);
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
  Scheduler(P& platform, ModuleList<Modules...> modules,
            SubscriberList<Subscribers> subscribers)
      : platform_(platform), logger_(platform, subscribers) {
    this->bind(*this);
    std::apply([&](auto*... module) { (Register(module), ...); },
               modules.items);
  }
  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;
  ~Scheduler() { platform_.quiesce(); }

  Status init() {
    {
      Guard guard(platform_);
      if (state_ == State::failed) return failure_;
      if (state_ != State::fresh) return Status::already_initialized;
      state_ = State::initializing;
    }
    Status status = Validate();
    if (status == Status::ok) {
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
    if (status != Status::ok) logger_.flush();
    return status;
  }

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
      if (logger_.dispatch()) continue;
      bool sleep = platform_.can_sleep();
      for (const auto& module : modules_) {
        ContextGuard context(platform_, {module.name, "can_sleep"});
        if (!module.sleep(module.object)) sleep = false;
      }
      Time deadline = kForever;
      {
        Guard guard(platform_);
        if (stop_requested_) continue;
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
    logger_.flush();
    {
      Guard guard(platform_);
      state_ = State::stopped;
    }
    return Status::ok;
  }

  Status stop() {
    Guard guard(platform_);
    if (state_ != State::running) return Status::not_running;
    if (platform_.can_stop()) {
      stop_requested_ = true;
      platform_.notify();
    }
    return Status::ok;
  }
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
  void minimum(Level level) { logger_.minimum(level); }
  Statistics<kTasks> snapshot() {
    Guard guard(platform_);
    auto snapshot = statistics_;
    snapshot.logs = logger_.counters();
    return snapshot;
  }
  void reset_statistics() {
    Guard guard(platform_);
    statistics_ = {};
    for (std::size_t index = 0; index < kTasks; ++index) {
      statistics_.tasks[index].module = tasks_[index].module_name;
      statistics_.tasks[index].task = tasks_[index].name;
    }
    logger_.reset();
  }
  void log_statistics() {
    auto stats = snapshot();
    this->log(Level::info,
              "Module/task | calls late | lateness min max avg (us)");
    for (const auto& task : stats.tasks) {
      this->log(
          Level::info, "%s/%s | %llu %llu | %llu %llu %llu %.1f", task.module,
          task.task, static_cast<unsigned long long>(task.executions),
          static_cast<unsigned long long>(task.late_starts),
          static_cast<unsigned long long>(task.max_lateness),
          static_cast<unsigned long long>(task.min_duration),
          static_cast<unsigned long long>(task.max_duration), task.average());
    }
    this->log(Level::info,
              "Overflow events=%llu timers=%llu logs=%llu truncated=%llu",
              static_cast<unsigned long long>(stats.event_overflows),
              static_cast<unsigned long long>(stats.timer_overflows),
              static_cast<unsigned long long>(stats.logs.dropped),
              static_cast<unsigned long long>(stats.logs.truncated));
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
  void Register(M* module) {
    static_assert(std::is_same_v<typename M::EventType, Event>);
    if (!module) {
      registration_error_ = true;
      return;
    }
    modules_[module_count_++] = {
        module, module->name(),
        [](void* self, InitStage stage) {
          return static_cast<M*>(self)->initialize(stage);
        },
        [](void* self, Event event) { static_cast<M*>(self)->receive(event); },
        [](void* self) { return static_cast<M*>(self)->permits_sleep(); }};
    module->bind(*this);
    constexpr auto descriptors = M::tasks();
    static_assert(descriptors.size() > 0);
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
      tasks_[task_count_] = {
          module,
          module->name(),
          module,
          [](void* object, std::size_t slot) {
            constexpr auto tasks = M::tasks();
            (static_cast<M*>(object)->*tasks[slot].callback)();
          },
          index,
          descriptors[index].name};
      statistics_.tasks[task_count_++] = {module->name(),
                                          descriptors[index].name};
      if (!descriptors[index].callback) registration_error_ = true;
      for (std::size_t previous = 0; previous < index; ++previous) {
        if (descriptors[previous].callback == descriptors[index].callback)
          registration_error_ = true;
      }
    }
  }
  Status Validate() {
    if (registration_error_) return Status::invalid_argument;
    for (std::size_t index = 0; index < modules_.size(); ++index) {
      const char* name = modules_[index].name;
      if (!name || !*name) return Status::invalid_argument;
      for (std::size_t previous = 0; previous < index; ++previous) {
        if (std::strcmp(name, modules_[previous].name) == 0)
          return Status::duplicate_name;
      }
    }
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
  Status schedule_slot(void* module, std::size_t index, Time delay, Mode mode) {
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
  Status cancel_slot(void* module, std::size_t index) {
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
  Status log_args(Level level, const char* format, std::va_list args) {
    return logger_.write(level, format, args);
  }
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
  Logger<LogCapacity, MessageSize, Subscribers, P> logger_;
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

template <typename Event, std::size_t Events = 32, std::size_t Timers = 16,
          std::size_t Logs = 32, std::size_t Message = 128, typename P,
          typename... Modules, std::size_t Subscribers>
auto make_scheduler(P& platform, ModuleList<Modules...> modules,
                    SubscriberList<Subscribers> subscribers) {
  return Scheduler<Event, ModuleList<Modules...>, SubscriberList<Subscribers>,
                   P, Events, Timers, Logs, Message>(platform, modules,
                                                     subscribers);
}
template <typename Event, std::size_t Events = 32, std::size_t Timers = 16,
          std::size_t Logs = 32, std::size_t Message = 128, typename P,
          typename... Modules>
auto make_scheduler(P& platform, ModuleList<Modules...> modules) {
  return make_scheduler<Event, Events, Timers, Logs, Message>(platform, modules,
                                                              SubscriberList{});
}
}  // namespace daveos::core
