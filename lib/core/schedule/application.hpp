#pragma once

#include <utility>

#include "core/command/command.hpp"
#include "core/command/source.hpp"
#include "scheduler.hpp"

namespace daveos::core {


  // Compile-time storage sizes, independent of optional logging/commands.
  struct Capacities {
    std::size_t events = 32;
    std::size_t timers = 16;
    std::size_t line = 256;
    std::size_t arguments = 8;
    // Maximum active task callbacks, including the outer task.
    std::size_t yield_depth = 4;
  };

  namespace detail {
    struct NoCommandSources {};

    template <typename Event, typename Modules, typename Sources,
              std::size_t LineCapacity, std::size_t ArgumentCapacity>
    class ApplicationCommands {
     public:
      ApplicationCommands(Modules modules, SchedulerInterface<Event>& scheduler,
                          Sources sources)
          : dispatcher_(modules, scheduler), sources_(sources) {}

      Status validate() const { return dispatcher_.validate(); }

      Status bind() { return dispatcher_.bind_sources(sources_); }

     private:
      CommandDispatcher<Event, Modules, LineCapacity, ArgumentCapacity>
          dispatcher_;
      Sources sources_;
    };

    template <typename Event, typename Modules, std::size_t LineCapacity,
              std::size_t ArgumentCapacity>
    class ApplicationCommands<Event, Modules, NoCommandSources, LineCapacity,
                              ArgumentCapacity> {
     public:
      ApplicationCommands(Modules, SchedulerInterface<Event>&,
                          NoCommandSources) {}

      Status validate() const { return Status::ok; }

      Status bind() { return Status::ok; }
    };
  }  // namespace detail

  // Owns scheduler/optional dispatcher; borrows all supplied application
  // objects. Construction is passive. Complete construction of borrowed objects
  // before init/run and keep them alive through destruction. Sources bind only
  // after successful stage2 completion. Lifecycle calls belong on this object,
  // on one thread; scheduler() exposes scheduling/diagnostics, not a second
  // lifecycle. Non-movable because the dispatcher refers to the owned
  // scheduler.
  template <typename Event, typename Modules, typename Logging, typename P,
            typename Sources = detail::NoCommandSources,
            std::size_t Events = 32, std::size_t Timers = 16,
            std::size_t LineCapacity = 256, std::size_t ArgumentCapacity = 8>
  class Application {
   public:
    Application(P& platform, Modules modules, Logging logging, Sources sources,
                std::size_t yield_depth = 4)
        : scheduler_(platform, modules, logging, yield_depth),
          commands_(modules, scheduler_, sources) {}

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // Command routes are checked before any module init() runs, so a
    // duplicate instance route starts no hardware. That failure is reported
    // like registration validation (null module).
    [[nodiscard]] Status init() {
      if (commands_failure_.status != Status::ok) {
        return commands_failure_.status;
      }
      auto status = commands_.validate();
      if (status != Status::ok) {
        commands_failure_ = {status, nullptr, InitStage::stage1};
        return status;
      }
      status = scheduler_.init();
      if (status == Status::ok) {
        status = commands_.bind();
        initialized_ = status == Status::ok;
      }
      return status;
    }

    [[nodiscard]] Status run() {
      if (used_) {
        return Status::already_run;
      }
      used_ = true;
      if (!initialized_) {
        auto status = init();
        if (status != Status::ok) {
          return status;
        }
      }
      return scheduler_.run();
    }

    auto& scheduler() { return scheduler_; }

    [[nodiscard]] InitializationFailure initialization_failure() {
      return commands_failure_.status != Status::ok
                 ? commands_failure_
                 : scheduler_.initialization_failure();
    }

   private:
    Scheduler<Event, Modules, Logging, P, Events, Timers> scheduler_;
    [[no_unique_address]] detail::ApplicationCommands<
        Event, Modules, Sources, LineCapacity, ArgumentCapacity>
        commands_;
    InitializationFailure commands_failure_;
    bool initialized_ = false;
    bool used_ = false;
  };

  // No logger or command machinery. Return directly for guaranteed copy
  // elision.
  template <typename Event = NoEvent, Capacities Capacity = {}, typename P,
            typename... M>
  auto make_application(P& platform, ModuleList<M...> modules) {
    return Application<Event, ModuleList<M...>, NoLogging, P,
                       detail::NoCommandSources, Capacity.events,
                       Capacity.timers>(platform, modules, {}, {},
                                        Capacity.yield_depth);
  }

  // Commands without logging. Sources remain externally owned and stable.
  template <typename Event = NoEvent, Capacities Capacity = {}, typename P,
            typename... M, std::size_t Sources>
  auto make_application(P& platform, ModuleList<M...> modules,
                        CommandSourceList<Sources> sources) {
    return Application<Event, ModuleList<M...>, NoLogging, P,
                       CommandSourceList<Sources>, Capacity.events,
                       Capacity.timers, Capacity.line, Capacity.arguments>(
        platform, modules, {}, sources, Capacity.yield_depth);
  }

  // Logging without commands. Disabled builds retain no logger attachment.
  template <typename Event = NoEvent, Capacities Capacity = {}, typename P,
            typename... M, typename L>
    requires LoggerFor<L, P>
  auto make_application(P& platform, ModuleList<M...> modules, L& logger) {
#if DAVEOS_LOGGING
    return Application<Event, ModuleList<M...>, LogService<L>, P,
                       detail::NoCommandSources, Capacity.events,
                       Capacity.timers>(
        platform, modules, LogService<L>(logger), {}, Capacity.yield_depth);
#else
    (void)logger;
    return make_application<Event, Capacity>(platform, modules);
#endif
  }

  // Logging and commands are independent; disabling logging keeps the sources.
  template <typename Event = NoEvent, Capacities Capacity = {}, typename P,
            typename... M, typename L, std::size_t Sources>
    requires LoggerFor<L, P>
  auto make_application(P& platform, ModuleList<M...> modules, L& logger,
                        CommandSourceList<Sources> sources) {
#if DAVEOS_LOGGING
    return Application<Event, ModuleList<M...>, LogService<L>, P,
                       CommandSourceList<Sources>, Capacity.events,
                       Capacity.timers, Capacity.line, Capacity.arguments>(
        platform, modules, LogService<L>(logger), sources,
        Capacity.yield_depth);
#else
    (void)logger;
    return make_application<Event, Capacity>(platform, modules, sources);
#endif
  }

  // Capacity-first shorthand defaults to NoEvent and preserves all service
  // combinations and validation from the explicit Event-first factories.
  template <Capacities Capacity, typename Event = NoEvent, typename... Args>
    requires requires(Args&&... args) {
               make_application<Event, Capacity>(std::forward<Args>(args)...);
             }
  auto make_application(Args&&... args) {
    return make_application<Event, Capacity>(std::forward<Args>(args)...);
  }


}  // namespace daveos::core
