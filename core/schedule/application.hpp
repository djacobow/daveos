#pragma once

#include "core/command/command.hpp"
#include "core/command/source.hpp"
#include "scheduler.hpp"

namespace daveos::core {


  namespace detail {
    struct NoCommandSources {};

    template <typename Event, typename Modules, typename Sources,
              std::size_t LineCapacity, std::size_t ArgumentCapacity>
    class ApplicationCommands {
     public:
      ApplicationCommands(Modules modules, SchedulerInterface<Event>& scheduler,
                          Sources sources)
          : dispatcher_(modules, scheduler), sources_(sources) {}

      void bind() { dispatcher_.bind_sources(sources_); }

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

      void bind() {}
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
            std::size_t LineCapacity = 256, std::size_t ArgumentCapacity = 16>
  class Application {
   public:
    Application(P& platform, Modules modules, Logging logging, Sources sources)
        : scheduler_(platform, modules, logging),
          commands_(modules, scheduler_, sources) {}

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    [[nodiscard]] Status init() {
      auto status = scheduler_.init();
      if (status == Status::ok) {
        commands_.bind();
        initialized_ = true;
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
      return scheduler_.initialization_failure();
    }

   private:
    Scheduler<Event, Modules, Logging, P, Events, Timers> scheduler_;
    [[no_unique_address]] detail::ApplicationCommands<
        Event, Modules, Sources, LineCapacity, ArgumentCapacity>
        commands_;
    bool initialized_ = false;
    bool used_ = false;
  };

  // No logger or command machinery. Return directly for guaranteed copy
  // elision.
  template <typename Event, std::size_t Events = 32, std::size_t Timers = 16,
            typename P, typename... M>
  auto make_application(P& platform, ModuleList<M...> modules) {
    return Application<Event, ModuleList<M...>, NoLogging, P,
                       detail::NoCommandSources, Events, Timers>(
        platform, modules, {}, {});
  }

  // Commands without logging. Sources remain externally owned and stable.
  template <typename Event, std::size_t Events = 32, std::size_t Timers = 16,
            std::size_t LineCapacity = 256, std::size_t ArgumentCapacity = 16,
            typename P, typename... M, std::size_t Sources>
  auto make_application(P& platform, ModuleList<M...> modules,
                        CommandSourceList<Sources> sources) {
    return Application<Event, ModuleList<M...>, NoLogging, P,
                       CommandSourceList<Sources>, Events, Timers, LineCapacity,
                       ArgumentCapacity>(platform, modules, {}, sources);
  }

  // Logging without commands. Disabled builds retain no logger attachment.
  template <typename Event, std::size_t Events = 32, std::size_t Timers = 16,
            typename P, typename... M, typename L>
    requires requires(L& logger) { logger.counters(); }
  auto make_application(P& platform, ModuleList<M...> modules, L& logger) {
#if DAVEOS_LOGGING
    return Application<Event, ModuleList<M...>, LogService<L>, P,
                       detail::NoCommandSources, Events, Timers>(
        platform, modules, LogService<L>(logger), {});
#else
    (void)logger;
    return make_application<Event, Events, Timers>(platform, modules);
#endif
  }

  // Logging and commands are independent; disabling logging keeps the sources.
  template <typename Event, std::size_t Events = 32, std::size_t Timers = 16,
            std::size_t LineCapacity = 256, std::size_t ArgumentCapacity = 16,
            typename P, typename... M, typename L, std::size_t Sources>
  auto make_application(P& platform, ModuleList<M...> modules, L& logger,
                        CommandSourceList<Sources> sources) {
#if DAVEOS_LOGGING
    return Application<Event, ModuleList<M...>, LogService<L>, P,
                       CommandSourceList<Sources>, Events, Timers, LineCapacity,
                       ArgumentCapacity>(platform, modules,
                                         LogService<L>(logger), sources);
#else
    (void)logger;
    return make_application<Event, Events, Timers, LineCapacity,
                            ArgumentCapacity>(platform, modules, sources);
#endif
  }


}  // namespace daveos::core
