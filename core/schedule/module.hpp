#pragma once

#include <array>
#include <concepts>
#include <cstdarg>
#include <cstdlib>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>

#include "core/command/arguments.hpp"
#include "core/event/event.hpp"
#include "core/foundation/duration.hpp"
#include "core/logging/log.hpp"
#include "core/platform/platform.hpp"

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
    Time period = 0;  // Zero leaves scheduling entirely to the application.
  };

  // Declarative repeat interval: validated at compile time, never rounded.
  // abort makes an invalid constant expression fail compilation, including in
  // embedded builds without exceptions. No runtime abort path is generated.
  template <typename M, DurationRep Rep, typename Period>
  consteval TaskDescriptor<M> periodic_task(
      const char* name, void (M::*callback)(),
      std::chrono::duration<Rep, Period> interval) {
    Time micros = 0;
    if (to_microseconds(interval, micros) != Status::ok || !micros) {
      std::abort();
    }
    return {name, callback, micros};
  }

// Register and repeat, first due one interval after normal dispatch starts.
#define DAVEOS_PERIODIC(ModuleType, function, interval)                       \
  ::daveos::core::periodic_task<ModuleType>(#function, &ModuleType::function, \
                                            interval)

// Register a task using its C++ function identifier as the displayed name.
#define DAVEOS_TASK(ModuleType, function)      \
  ::daveos::core::TaskDescriptor<ModuleType> { \
#function, &ModuleType::function           \
  }

  // Validate compile-time task selection after the derived module is complete.
  template <typename M, auto Function>
  consteval std::size_t TaskIndex() {
    constexpr auto tasks = M::tasks();
    constexpr auto index = [&] {
      for (std::size_t i = 0; i < tasks.size(); ++i) {
        if (tasks[i].callback == Function) {
          return i;
        }
      }
      return tasks.size();
    }();
    static_assert(index < tasks.size(),
                  "task callback must be registered in tasks()");
    return index;
  }

  template <typename Event, typename Modules, std::size_t LineCapacity = 256,
            std::size_t ArgumentCapacity = 8>
  class CommandDispatcher;

  // Non-owning erased reference, independent of module lists and capacities.
  // Only this boundary uses indirect service calls; there are no virtual
  // methods. The scheduler binds this interface before module initialization.
  // Do not invoke an unbound interface or retain it beyond the owning
  // scheduler's lifetime.
  template <typename Event>
  class SchedulerInterface {
    static_assert(core::EventType<Event>,
                  "events require a std::variant of unique, trivially "
                  "copyable, nonthrowing payload types");

    struct Operations {
      Status (*schedule)(void*, void*, std::size_t, Time, Mode);
      Status (*cancel)(void*, void*, std::size_t);
      Status (*post)(void*, const Event&, void*);
      Status (*timer)(void*, Time, const TimerCallback&);
      Status (*cancel_timer)(void*, const TimerCallback&);
      Status (*stop)(void*);
      Status (*yield)(void*);
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
            [](void* self, const Event& event, void* sender) {
              return static_cast<Impl*>(self)->post(event, sender);
            },
            [](void* self, Time delay, const TimerCallback& callback) {
              return static_cast<Impl*>(self)->TimerSlot(delay, callback);
            },
            [](void* self, const TimerCallback& callback) {
              return static_cast<Impl*>(self)->cancel_timer(callback);
            },
            [](void* self) { return static_cast<Impl*>(self)->stop(); },
            [](void* self) { return static_cast<Impl*>(self)->yield(); },
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
    // Function must appear in M::tasks() (checked at compile time). Repeat uses
    // delay as both initial delay and interval. Zero is allowed only for once.
    // Before run(), delays start at the common run start time; while running
    // they start at this call. Inexact or out-of-range durations return
    // invalid_argument without replacing pending work. Safe from interrupts.
    template <auto Function, typename M, DurationRep Rep, typename Period>
      requires ModuleFor<M, Event>
    [[nodiscard]] Status schedule(M& module,
                                  std::chrono::duration<Rep, Period> delay,
                                  Mode mode = Mode::once) {
      Time micros;
      const auto status = to_microseconds(delay, micros);
      return status == Status::ok
                 ? operations_->schedule(object_, &module,
                                         TaskIndex<M, Function>(), micros, mode)
                 : status;
    }

    // Cancel pending execution (not an already executing callback). Returns
    // not_found for an inactive task; interrupt callers are rejected.
    template <auto Function, typename M>
      requires ModuleFor<M, Event>
    [[nodiscard]] Status cancel(M& module) {
      return operations_->cancel(object_, &module, TaskIndex<M, Function>());
    }

    // Copy a variant broadcast, optionally excluding a registered sender.
    // Accepted before run() and from interrupts; full rejects the newest event.
    Status post(const Event& event, void* sender = nullptr) {
      return operations_->post(object_, event, sender);
    }

    template <typename Payload>
      requires(!std::same_as<Payload, Event>)
    Status post(const Payload& payload, void* sender = nullptr) {
      static_assert(
          EventAlternative<Payload, Event>,
          "posted payload must be an alternative of the application variant");
      return post(Event{std::in_place_type<Payload>, payload}, sender);
    }

    // Interrupt-context one-shot, available only while running. Delay must be
    // positive and callback non-null. Reusing callback identity replaces its
    // timer.
    template <DurationRep Rep, typename Period>
    [[nodiscard]] Status timer(std::chrono::duration<Rep, Period> delay,
                               const TimerCallback& callback) {
      Time micros;
      const auto status = to_microseconds(delay, micros);
      return status == Status::ok
                 ? operations_->timer(object_, micros, callback)
                 : status;
    }

    template <auto Function, typename Object, DurationRep Rep, typename Period>
    [[nodiscard]] Status timer(Object& object,
                               std::chrono::duration<Rep, Period> delay) {
      return timer(delay, TimerCallback::bind<Function>(object));
    }

    template <auto Function, typename Object>
    [[nodiscard]] Status cancel_timer(Object& object) {
      return cancel_timer(TimerCallback::bind<Function>(object));
    }

    // Cancel by callback identity, including from interrupts; not_found if
    // absent.
    Status cancel_timer(const TimerCallback& callback) {
      return operations_->cancel_timer(object_, callback);
    }

    // Request cooperative stop if supported; returns not_running outside run().
    Status stop() { return operations_->stop(object_); }

    // Task-call-chain only. Run at most one other due task, without sleeping
    // or dispatching events/logs. Never wait for resources held by a suspended
    // ancestor. empty means no eligible task; not_running means stop requested.
    // The caller must still finish/cancel its own borrowed-buffer operations
    // before returning. Fake time is not advanced by yield().
    Status yield() {
      return operations_ ? operations_->yield(object_)
                         : Status::invalid_context;
    }

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

    template <typename, typename>
    friend class Module;

    // Callback scope bridge; validates running state/interrupt context.
    Status Invoke(Context context, Status (*callback)(void*), void* argument) {
      return operations_->invoke(object_, context, callback, argument);
    }

    void* object_ = nullptr;
    const Operations* operations_ = nullptr;
  };

  // CRTP module defaults. Derived provides static constexpr name(), the default
  // name for its instances; tasks(), commands(), command_prefix(), lifecycle,
  // event and sleep hooks are optional. A type that can be registered more
  // than once passes a distinct instance name to the protected constructor.
  // Callbacks run to completion on the scheduler thread; asynchronous work
  // must schedule a task or maintain its own state. Do not move a registered
  // module. Its name and object must outlive the scheduler.
  template <typename Derived, typename Event = NoEvent>
  class Module {
    static_assert(core::EventType<Event>,
                  "events require a std::variant of unique, trivially "
                  "copyable, nonthrowing payload types");

   public:
    using EventType = Event;

    static constexpr auto tasks() {
      return std::array<TaskDescriptor<Derived>, 0>{};
    }

    static constexpr auto commands() {
      return std::array<CommandDescriptor<Derived>, 0>{};
    }

    // Optional fixed command route for every instance of a type. The default
    // (null) routes commands by instance name.
    static constexpr const char* command_prefix() { return nullptr; }

    // Instance identity for logs, statistics, diagnostics and command routing.
    // Read only from init() onward, never during static construction.
    const char* module_name() const { return name_ ? name_ : Derived::name(); }

    const char* command_route() const {
      return Derived::command_prefix() ? Derived::command_prefix()
                                       : module_name();
    }

    // stage1 is independent setup; stage2 may use other modules' stage1
    // results.
    Status init(InitStage) { return Status::ok; }

    // Broadcast reception order between modules is unspecified.
    void on_event(const Event&) {}

    // Named handlers are optional; alternatives absent from this tuple are
    // ignored.
    static constexpr auto events() { return std::tuple{}; }

    // A veto prevents sleeping but does not prevent the scheduler from waiting.
    bool can_sleep() { return true; }

    // Bound by scheduler init() before any stage1 callback. Not available in
    // constructors; valid throughout both initialization stages and dispatch.
    SchedulerInterface<Event>& scheduler() { return *scheduler_; }

    // Self-scheduling helpers validate the callback against tasks() at compile
    // time. Delays are integral chrono durations; use core::Microseconds for a
    // value already measured in Time.
    template <auto Function, DurationRep Rep, typename Period>
    [[nodiscard]] Status schedule(std::chrono::duration<Rep, Period> delay,
                                  Mode mode = Mode::once) {
      return scheduler().template schedule<Function>(
          static_cast<Derived&>(*this), delay, mode);
    }

    template <auto Function>
    [[nodiscard]] Status cancel() {
      return scheduler().template cancel<Function>(
          static_cast<Derived&>(*this));
    }

    template <auto Function, DurationRep Rep, typename Period>
    [[nodiscard]] Status timer(std::chrono::duration<Rep, Period> delay) {
      return scheduler().template timer<Function>(static_cast<Derived&>(*this),
                                                  delay);
    }

    template <auto Function>
    [[nodiscard]] Status cancel_timer() {
      return scheduler().template cancel_timer<Function>(
          static_cast<Derived&>(*this));
    }

    void bind(SchedulerInterface<Event>& scheduler) { scheduler_ = &scheduler; }

    // CRTP forwarding keeps callback dispatch statically typed at registration.
    Status initialize(InitStage stage) {
      static_assert(
          requires(Derived & module) {
            { module.init(stage) } -> std::same_as<Status>;
          }, "module init(InitStage) must return core::Status");
      return static_cast<Derived*>(this)->init(stage);
    }

    void receive(const Event& event) {
      static constexpr auto handlers = Derived::events();
      constexpr bool custom_visitor =
          !std::is_same_v<decltype(&Derived::on_event),
                          decltype(&Module::on_event)>;
      static_assert(
          !custom_visitor || std::tuple_size_v<decltype(handlers)> == 0,
          "use either events() registration or on_event(const Event&), not "
          "both");
      if constexpr (custom_visitor) {
        using Visitor = detail::EventMember<decltype(&Derived::on_event)>;
        static_assert(std::same_as<typename Visitor::Payload, Event>,
                      "on_event must take const Event&");
        static_cast<Derived*>(this)->on_event(event);
      } else {
        static_assert(detail::ValidEventHandlers<Derived, Event>(handlers),
                      "invalid event handler metadata");
        std::visit(
            [&](const auto& payload) {
              std::apply(
                  [&](const auto&... handler) {
                    (Deliver(handler, payload), ...);
                  },
                  handlers);
            },
            event);
      }
    }

    bool permits_sleep() {
      static_assert(
          requires(Derived & module) {
            { module.can_sleep() } -> std::same_as<bool>;
          }, "module can_sleep() must return bool");
      return static_cast<Derived*>(this)->can_sleep();
    }

   protected:
    Module() = default;

    // Borrowed instance name; must outlive the scheduler. Null keeps the
    // type's default name.
    explicit Module(const char* name) : name_(name) {}

    ~Module() = default;

   private:
    template <typename Handler, typename Payload>
    void Deliver(const Handler& handler, const Payload& payload) {
      if constexpr (std::same_as<typename Handler::Payload, Payload>) {
        struct Call {
          Derived& owner;
          const Payload& payload;
        } call{*static_cast<Derived*>(this), payload};

        scheduler_->Invoke(
            {module_name(), handler.name},
            [](void* argument) {
              auto& call = *static_cast<Call*>(argument);
              (call.owner.*Handler::callback)(call.payload);
              return Status::ok;
            },
            &call);
      }
    }

    SchedulerInterface<Event>* scheduler_ = nullptr;
    const char* name_ = nullptr;
  };

  namespace detail {
    // True if a different module type shares A's default name. Repeated
    // instances of one type are checked by name at scheduler init() instead.
    template <typename A, typename... Modules>
    consteval bool DefaultNameClash() {
      return ((!std::is_same_v<A, Modules> &&
               EqualName(A::name(), Modules::name())) ||
              ...);
    }
  }  // namespace detail

  // Non-owning module pointers whose concrete types determine compile-time
  // storage. A type may appear more than once when its instances have
  // distinct names.
  template <typename... Modules>
  struct ModuleList {
    static_assert((requires {
                     { Modules::name() } -> std::convertible_to<const char*>;
                     typename std::bool_constant<(Modules::name() != nullptr)>;
                   } && ...),
                  "modules must provide static constexpr const char* name()");
    static_assert(((*Modules::name() != '\0') && ...) &&
                      !(detail::DefaultNameClash<Modules, Modules...>() || ...),
                  "module default names must be nonempty and differ between "
                  "module types (case-insensitive)");
    std::tuple<Modules*...> items;

    explicit ModuleList(Modules*... modules) : items(modules...) {}
  };

  template <typename... Modules>
  ModuleList(Modules*...) -> ModuleList<Modules...>;


}  // namespace daveos::core
