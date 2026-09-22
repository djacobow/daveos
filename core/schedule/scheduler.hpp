#pragma once

#include <array>
#include <cstring>
#include <type_traits>
#include <utility>

#include "core/logging/log_format.hpp"
#include "core/queue/queue.hpp"
#include "core/schedule/module.hpp"
#include "core/state_machine/state_machine.hpp"
#include "progress.h"

namespace daveos::core {


  // Completed task iterations since reset. Durations/lateness are microseconds;
  // min/max/average are zero before the first completion. Any late start
  // counts. Name pointers are borrowed from the registered module/task
  // descriptors.
  struct TaskStatistics {
    const char* module = "";
    const char* task = "";
    std::uint64_t executions = 0;
    std::uint64_t late_starts = 0;
    Time max_lateness = 0;
    Time min_duration = 0;
    Time max_duration = 0;
    Time total_duration = 0;
    // Elapsed duration includes nested tasks. Self time excludes them but still
    // includes interrupt time and any busy waiting in this callback.
    Time total_self_duration = 0;
    Time total_nested_duration = 0;

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
    std::uint64_t invalid_yields = 0;
    std::uint64_t yield_depth_errors = 0;
  };

  // First initialization failure. module==nullptr denotes registration
  // validation (stage is then irrelevant). Names are borrowed. Available even
  // without logging, and retained after subsequent init/run attempts.
  struct InitializationFailure {
    Status status = Status::ok;
    const char* module = nullptr;
    InitStage stage = InitStage::stage1;
  };

  // Fixed-storage cooperative scheduler. Prefer make_scheduler() for deduction.
  // Event is the application's payload variant. ModuleList determines
  // module/task capacity. Logging is an optional borrowed service; the
  // scheduler owns no log buffers. Platform, modules, logger and name strings
  // must outlive this object. At least one module is required; task arrays may
  // be empty. One thread owns init()/run()/destruction; these are never ISR
  // entry points.
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

    // Lifecycle transitions run under the existing platform guard. Each tick
    // is a lifecycle request, not a task-dispatch iteration.
    class Lifecycle
        : public StateMachine<Lifecycle, State, State::fresh,
                              static_cast<std::size_t>(State::failed) + 1> {
      friend class StateMachine<Lifecycle, State, State::fresh,
                                static_cast<std::size_t>(State::failed) + 1>;

     public:
      enum class Request { initialize, ready, fail, run, stop, stopped };

     private:
      void Step(State cs, State& ns, Request request) {
        switch (cs) {
          case State::fresh:
            if (request == Request::initialize) {
              ns = State::initializing;
            }
            break;
          case State::initializing:
            if (request == Request::ready) {
              ns = State::ready;
            } else if (request == Request::fail) {
              ns = State::failed;
            }
            break;
          case State::ready:
            if (request == Request::run) {
              ns = State::running;
            }
            break;
          case State::running:
            if (request == Request::stop) {
              ns = State::stopping;
            }
            break;
          case State::stopping:
            if (request == Request::stopped) {
              ns = State::stopped;
            }
            break;
          case State::stopped:
          case State::failed:
            break;
        }
      }
    };

    struct Registration {
      void* object = nullptr;
      const char* name = "";
      void (*bind)(void*, SchedulerInterface<Event>&) = nullptr;
      Status (*init)(void*, InitStage) = nullptr;
      void (*event)(void*, const Event&) = nullptr;
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
      bool executing = false;
      Mode mode = Mode::once;
      Time interval = 0;
      Time due = 0;
      Time default_period = 0;
      bool explicitly_scheduled = false;
      Time first_due = 0;
      std::uint64_t completed = 0;
      std::uint64_t generation = 0;
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
    using SchedulerInterface<Event>::timer;
    using SchedulerInterface<Event>::post;
    using SchedulerInterface<Event>::cancel_timer;

    // Store references and static descriptors only; binding and callbacks wait
    // for init(), after all application objects have been constructed.
    Scheduler(P& platform, ModuleList<Modules...> modules, Logging logging = {},
              std::size_t yield_depth = 4)
        : platform_(platform), logging_(logging), yield_limit_(yield_depth) {
      this->bind(*this);
      std::apply([&](auto*... module) { (Register(module), ...); },
                 modules.items);
    }

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    // Quiesce platform callbacks before releasing scheduler-owned storage.
    ~Scheduler() { platform_.quiesce(); }

    // Validate registration, then run stage1 for every module followed by
    // stage2. First failure is terminal: discard queued tasks/events and flush
    // logs. Successful initialization is never repeated; later calls report an
    // error.
    [[nodiscard]] Status init() {
      {
        Guard guard(platform_);
        if (lifecycle_.state() == State::failed) {
          return failure_;
        }
        if (lifecycle_.state() != State::fresh) {
          return Status::already_initialized;
        }
        (void)lifecycle_.tick(Lifecycle::Request::initialize);
      }
      Status status = Validate();
      if (status == Status::ok) {
        for (const auto& module : modules_) {
          module.bind(module.object, *this);
        }
        {
          Guard guard(platform_);
          for (auto& task : tasks_) {
            if (task.default_period && !task.explicitly_scheduled) {
              task.active = true;
              task.mode = Mode::repeat;
              task.interval = task.due = task.default_period;
            }
          }
        }
        for (auto stage : {InitStage::stage1, InitStage::stage2}) {
          for (const auto& module : modules_) {
            ContextGuard context(
                platform_,
                {module.name, "init", CallbackKind::initialization, this});
            status = module.init(module.object, stage);
            if (status != Status::ok) {
              Guard guard(platform_);
              initialization_failure_ = {status, module.name, stage};
              break;
            }
          }
          if (status != Status::ok) {
            break;
          }
        }
      }
      {
        Guard guard(platform_);
        if (status == Status::ok) {
          (void)lifecycle_.tick(Lifecycle::Request::ready);
        } else {
          (void)lifecycle_.tick(Lifecycle::Request::fail);
          failure_ = status;
          initialization_failure_.status = status;
          for (auto& task : tasks_) {
            task.active = false;
          }
          event_count_ = 0;
        }
      }
      if constexpr (kHasLogging) {
        if (status != Status::ok) {
          const auto failure = initialization_failure();
          // Subscriber delivery remains outside scheduler locks. Failed
          // transports may not deliver this; the snapshot is always retained.
          this->log(Level::error, "Initialization failed: %s (%s, stage %u)",
                    enum_name(status),
                    failure.module ? failure.module : "registration",
                    failure.module
                        ? (failure.stage == InitStage::stage1 ? 1u : 2u)
                        : 0u);
          logging_.flush();
        }
      }
      return status;
    }

    [[nodiscard]] InitializationFailure initialization_failure() {
      Guard guard(platform_);
      return initialization_failure_;
    }

    // Single-use dispatch loop; initialize automatically when still fresh.
    // Due tasks/events run earliest-first, with unspecified tie order.
    // Repeating tasks retain every overdue iteration and advance from their
    // scheduled times. Logs dispatch one record at a time when no task/event is
    // due. Idle considers both task and timer deadlines and sleeps only when
    // every module permits it. A supported stop waits for active callbacks,
    // discards work, then flushes logs.
    [[nodiscard]] Status run() {
      bool initialize = false;
      {
        Guard guard(platform_);
        if (used_) {
          return Status::already_run;
        }
        used_ = true;
        if (lifecycle_.state() == State::failed) {
          return failure_;
        }
        if (lifecycle_.state() != State::fresh &&
            lifecycle_.state() != State::ready) {
          return Status::busy;
        }
        initialize = lifecycle_.state() == State::fresh;
      }
      if (initialize) {
        Status status = init();
        if (status != Status::ok) {
          return status;
        }
      }
      {
        Guard guard(platform_);
        (void)lifecycle_.tick(Lifecycle::Request::run);
        Time start = platform_.now();
        for (auto& task : tasks_) {
          if (task.active) {
            task.due = After(start, task.interval);
            task.first_due = task.due;
          }
        }
      }
      while (true) {
        auto observed = platform_.sequence();
        std::size_t task_index = kTasks;
        EventRecord event;
        bool has_event = false;
        Time scheduled = 0;
        std::uint64_t generation = 0;
        {
          Guard guard(platform_);
          if (stop_requested_) {
            break;
          }
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
            TakeTask(task_index, scheduled, generation);
          }
        }
        if (has_event) {
          for (const auto& module : modules_) {
            if (module.object == event.sender) {
              continue;
            }
            ContextGuard context(platform_, {module.name, "on_event",
                                             CallbackKind::event, this});
            module.event(module.object, event.value);
          }
          continue;
        }
        if (task_index < kTasks) {
          RunTask(task_index, scheduled, generation);
          continue;
        }
        if constexpr (kHasLogging) {
          if (logging_.dispatch()) {
            continue;
          }
        }
        bool sleep = platform_.can_sleep();
        for (const auto& module : modules_) {
          ContextGuard context(
              platform_, {module.name, "can_sleep", CallbackKind::idle, this});
          if (!module.sleep(module.object)) {
            sleep = false;
          }
        }
        Time deadline = kForever;
        {
          Guard guard(platform_);
          if (stop_requested_) {
            continue;
          }
          if constexpr (kHasLogging) {
            if (!logging_.empty()) {
              continue;
            }
          }
          for (const auto& task : tasks_) {
            if (task.active && task.due < deadline) {
              deadline = task.due;
            }
          }
          timers_[TimerCapacity] = {deadline != kForever, deadline, nullptr};
          for (const auto& timer : timers_) {
            if (timer.active && timer.due < deadline) {
              deadline = timer.due;
            }
          }
          Rearm();
        }
        platform_.idle(deadline, sleep, observed);
      }
      {
        Guard guard(platform_);
        (void)lifecycle_.tick(Lifecycle::Request::stop);
        for (auto& timer : timers_) {
          timer.active = false;
        }
        platform_.disarm();
        for (auto& task : tasks_) {
          task.active = false;
        }
        event_count_ = 0;
      }
      platform_.quiesce();
      if constexpr (kHasLogging) {
        logging_.flush();
      }
      {
        Guard guard(platform_);
        (void)lifecycle_.tick(Lifecycle::Request::stopped);
      }
      return Status::ok;
    }

    // Nested dispatch on the same C++ stack. Context identity is per execution
    // thread on host/fake; hardware also rejects every exception/ISR context.
    Status yield() {
      std::size_t selected = kTasks;
      Time scheduled = 0;
      std::uint64_t generation = 0;
      {
        Guard guard(platform_);
        const auto context = platform_.context();
        if (platform_.in_interrupt() || context.kind != CallbackKind::task ||
            context.scheduler != this) {
          ++statistics_.invalid_yields;
          return Status::invalid_context;
        }
        if (stop_requested_ || lifecycle_.state() != State::running) {
          return Status::not_running;
        }
        const auto now = platform_.now();
        Time earliest = kForever;
        for (std::size_t i = 0; i < kTasks; ++i) {
          const auto& task = tasks_[i];
          if (task.active && !task.executing && task.due <= now &&
              task.due < earliest) {
            earliest = task.due;
            selected = i;
          }
        }
        if (selected == kTasks) {
          return Status::empty;
        }
        if (task_depth_ >= yield_limit_) {
          ++statistics_.yield_depth_errors;
          return Status::depth_limit;
        }
        TakeTask(selected, scheduled, generation);
      }
      RunTask(selected, scheduled, generation);
      return Status::ok;
    }

    // ISR-safe cooperative request; does not interrupt the current callback or
    // partially delivered broadcast. Platforms may make this a successful
    // no-op.
    Status stop() {
      Guard guard(platform_);
      if (lifecycle_.state() != State::running) {
        return Status::not_running;
      }
      if (platform_.can_stop()) {
        stop_requested_ = true;
        platform_.notify();
      }
      return Status::ok;
    }

    // Queue a timestamped broadcast. sender, if supplied, must be registered
    // and is excluded from reception. Queue overflow increments
    // event_overflows.
    Status post(const Event& value, void* sender = nullptr) {
      Guard guard(platform_);
      if (!AcceptsWork()) {
        return Status::not_running;
      }
      if (value.valueless_by_exception()) {
        return Status::invalid_argument;
      }
      if (sender && !Contains(sender)) {
        return Status::not_found;
      }
      if (event_count_ == EventCapacity) {
        ++statistics_.event_overflows;
        return Status::full;
      }
      events_[event_count_++] = {value, sender, platform_.now()};
      platform_.notify();
      return Status::ok;
    }

    // ISR-safe cancellation of a pending callback, identified by function
    // pointer.
    Status cancel_timer(const TimerCallback& callback) {
      Guard guard(platform_);
      if (lifecycle_.state() != State::running) {
        return Status::not_running;
      }
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

    // Synchronized copy, available in every lifecycle state, including from
    // ISRs. Callers own the values, but module/task name strings remain
    // borrowed.
    Statistics<kTasks> snapshot() {
      Guard guard(platform_);
      return statistics_;
    }

    // Health snapshots do not share counters with reset_statistics(). The
    // currently executing callback is not complete yet; callers allow an
    // explicit completion grace instead of counting it early.
    Progress<kTasks> progress() {
      Guard guard(platform_);
      Progress<kTasks> result;
      result.running = lifecycle_.state() == State::running;
      result.now = platform_.now();
      for (std::size_t i = 0; i < kTasks; ++i) {
        const auto& task = tasks_[i];
        result.tasks[i] = {task.module_name,
                           task.name,
                           task.active && task.mode == Mode::repeat,
                           task.first_due,
                           task.interval,
                           task.completed,
                           task.generation};
      }
      return result;
    }

    // Reset timing/counters while retaining names, pending work and buffered
    // logs. An in-progress task records its whole iteration when it
    // subsequently finishes.
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
                "Module/task | calls late | lateness min max avg | self-total "
                "nested-total (us)");
      for (const auto& task : stats.tasks) {
        this->log(Level::info, "%s/%s | %s %s | %s %s %s %s | %s %s",
                  task.module, task.task, LogUnsigned(task.executions).c_str(),
                  LogUnsigned(task.late_starts).c_str(),
                  LogUnsigned(task.max_lateness).c_str(),
                  LogUnsigned(task.min_duration).c_str(),
                  LogUnsigned(task.max_duration).c_str(),
                  LogAverage(task.total_duration, task.executions).c_str(),
                  LogUnsigned(task.total_self_duration).c_str(),
                  LogUnsigned(task.total_nested_duration).c_str());
      }
      this->log(Level::info,
                "Overflow events=%s timers=%s; yield context=%s depth=%s",
                LogUnsigned(stats.event_overflows).c_str(),
                LogUnsigned(stats.timer_overflows).c_str(),
                LogUnsigned(stats.invalid_yields).c_str(),
                LogUnsigned(stats.yield_depth_errors).c_str());
#endif
    }

   private:
    friend class SchedulerInterface<Event>;

    // Multiplex a positive-delay callback over the one platform timer. Reached
    // only through SchedulerInterface::timer after chrono conversion. Safe from
    // interrupts, but rejected before run(). Replacing a callback needs no new
    // slot; a new callback at capacity fails and increments timer_overflows.
    Status TimerSlot(Time delay, const TimerCallback& callback) {
      Guard guard(platform_);
      if (lifecycle_.state() != State::running) {
        return Status::not_running;
      }
      if (!delay || !callback) {
        return Status::invalid_argument;
      }
      std::size_t slot = TimerCapacity;
      for (std::size_t index = 0; index < TimerCapacity; ++index) {
        if (timers_[index].active && timers_[index].callback == callback) {
          slot = index;
          break;
        }
        if (!timers_[index].active) {
          slot = index;
        }
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

    struct Frame {
      Time nested = 0;
    };

    // Caller holds the platform guard. Rescheduling/cancellation during the
    // callback wins through the existing generation mechanism.
    void TakeTask(std::size_t index, Time& scheduled,
                  std::uint64_t& generation) {
      auto& task = tasks_[index];
      scheduled = task.due;
      generation = task.generation;
      task.executing = true;
      if (task.mode == Mode::repeat) {
        task.due = After(task.due, task.interval);
      } else {
        task.active = false;
      }
    }

    void RunTask(std::size_t index, Time scheduled, std::uint64_t generation) {
      auto& task = tasks_[index];
      Frame frame;
      auto* parent = frame_;
      frame_ = &frame;
      ++task_depth_;
      const Time start = platform_.now();
      {
        ContextGuard context(
            platform_, {task.module_name, task.name, CallbackKind::task, this});
        task.invoke(task.object, task.index);
      }
      const Time duration = platform_.now() - start;
      --task_depth_;
      frame_ = parent;
      if (parent) {
        parent->nested += duration;
      }
      Guard guard(platform_);
      task.executing = false;
      auto& stats = statistics_.tasks[index];
      if (task.generation == generation) {
        ++task.completed;
      }
      if (!stats.executions || duration < stats.min_duration) {
        stats.min_duration = duration;
      }
      if (duration > stats.max_duration) {
        stats.max_duration = duration;
      }
      ++stats.executions;
      stats.total_duration += duration;
      stats.total_self_duration += duration - frame.nested;
      stats.total_nested_duration += frame.nested;
      if (start > scheduled) {
        ++stats.late_starts;
        if (start - scheduled > stats.max_lateness) {
          stats.max_lateness = start - scheduled;
        }
      }
    }

    bool AcceptsWork() const {
      return lifecycle_.state() == State::fresh ||
             lifecycle_.state() == State::initializing ||
             lifecycle_.state() == State::ready ||
             lifecycle_.state() == State::running;
    }

    bool Contains(void* module) const {
      for (const auto& entry : modules_) {
        if (entry.object == module) {
          return true;
        }
      }
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
          [](void* self, const Event& event) {
            static_cast<M*>(self)->receive(event);
          },
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
        tasks_[task_count_].default_period = descriptors[index].period;
        statistics_.tasks[task_count_++] = {M::name(), descriptors[index].name};
        if (!descriptors[index].callback ||
            descriptors[index].period == kForever) {
          registration_error_ = true;
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
          if (descriptors[previous].callback == descriptors[index].callback) {
            registration_error_ = true;
          }
        }
      }
    }

    Status Validate() {
      if constexpr (kHasLogging) {
        if (!logging_.uses_platform(platform_)) {
          return Status::invalid_argument;
        }
      }
      if (registration_error_) {
        return Status::invalid_argument;
      }
      for (std::size_t index = 0; index < kTasks; ++index) {
        if (!tasks_[index].name || !*tasks_[index].name) {
          return Status::invalid_argument;
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
          if (tasks_[index].module == tasks_[previous].module &&
              std::strcmp(tasks_[index].name, tasks_[previous].name) == 0) {
            return Status::duplicate_name;
          }
        }
      }
      return Status::ok;
    }

    Status ScheduleSlot(void* module, std::size_t index, Time delay,
                        Mode mode) {
      Guard guard(platform_);
      if (!AcceptsWork()) {
        return Status::not_running;
      }
      if (mode == Mode::repeat && !delay) {
        return Status::invalid_argument;
      }
      for (auto& task : tasks_) {
        if (task.module == module && task.index == index) {
          task.active = true;
          task.explicitly_scheduled = true;
          task.interval = delay;
          task.mode = mode;
          task.due = lifecycle_.state() == State::running
                         ? After(platform_.now(), delay)
                         : delay;
          task.first_due = task.due;
          task.completed = 0;
          ++task.generation;
          platform_.notify();
          return Status::ok;
        }
      }
      return Status::not_found;
    }

    Status CancelSlot(void* module, std::size_t index) {
      if (platform_.in_interrupt()) {
        return Status::invalid_argument;
      }
      Guard guard(platform_);
      if (!AcceptsWork()) {
        return Status::not_running;
      }
      for (auto& task : tasks_) {
        if (task.module == module && task.index == index) {
          if (!task.active) {
            return Status::not_found;
          }
          task.active = false;
          task.explicitly_scheduled = true;
          ++task.generation;
          platform_.notify();
          return Status::ok;
        }
      }
      return Status::not_found;
    }

    Status Invoke(Context context, Status (*callback)(void*), void* argument) {
      if (platform_.in_interrupt()) {
        return Status::invalid_argument;
      }
      {
        Guard guard(platform_);
        if (lifecycle_.state() != State::running) {
          return Status::not_running;
        }
      }
      context.kind = CallbackKind::command;
      context.scheduler = this;
      ContextGuard guard(platform_, context);
      return callback(argument);
    }

    Status LogArgs(Level level, const char* format, std::va_list args) {
      return logging_.write(level, format, args);
    }

    // Called under the platform guard; the final slot is the scheduler's own
    // wake timer.
    void Rearm() {
      Time earliest = kForever;
      for (const auto& timer : timers_) {
        if (timer.active && timer.due < earliest) {
          earliest = timer.due;
        }
      }
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
    // callbacks can safely rearm/cancel timers. User code runs outside our
    // guard.
    void Fire() {
      while (true) {
        TimerCallback callback = nullptr;
        {
          Guard guard(platform_);
          if (lifecycle_.state() != State::running) {
            return;
          }
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
        if (callback) {
          callback();
        }
        platform_.notify();
      }
    }

    Frame* frame_ = nullptr;
    std::size_t task_depth_ = 0;
    P& platform_;
    [[no_unique_address]] Logging logging_;
    std::array<Registration, sizeof...(Modules)> modules_{};
    std::array<Task, kTasks> tasks_{};
    std::array<EventRecord, EventCapacity> events_{};
    std::array<Timer, TimerCapacity + 1> timers_{};
    Statistics<kTasks> statistics_{};
    std::size_t module_count_ = 0, task_count_ = 0, event_count_ = 0;
    bool registration_error_ = false, used_ = false, stop_requested_ = false;
    const std::size_t yield_limit_;
    Lifecycle lifecycle_;
    Status failure_ = Status::initialization_failed;
    InitializationFailure initialization_failure_;
  };

  // Deduce module/platform types; optional sizes are event and timer slots.
  // With no logger there is no logging storage or work. An attached logger is
  // borrowed and must use the same platform and outlive the scheduler.
  template <typename Event = NoEvent, std::size_t Events = 32,
            std::size_t Timers = 16, typename P, typename... Modules>
  auto make_scheduler(P& platform, ModuleList<Modules...> modules,
                      std::size_t yield_depth = 4) {
    return Scheduler<Event, ModuleList<Modules...>, NoLogging, P, Events,
                     Timers>(platform, modules, {}, yield_depth);
  }

  template <typename Event = NoEvent, std::size_t Events = 32,
            std::size_t Timers = 16, typename P, typename... Modules,
            typename L>
    requires LoggerFor<L, P>
  auto make_scheduler(P& platform, ModuleList<Modules...> modules, L& logger,
                      std::size_t yield_depth = 4) {
#if DAVEOS_LOGGING
    return Scheduler<Event, ModuleList<Modules...>, LogService<L>, P, Events,
                     Timers>(platform, modules, LogService<L>(logger),
                             yield_depth);
#else
    (void)logger;
    return make_scheduler<Event, Events, Timers>(platform, modules,
                                                 yield_depth);
#endif
  }


}  // namespace daveos::core
