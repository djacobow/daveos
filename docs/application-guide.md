# Writing an application

Start from [`starters/application`](../starters/application/README.md) for a
separate Meson project with a host executable and a fake-time test. The framework
still uses explicit ownership and fixed capacities: constructors store references
and metadata, all modules finish stage1 before any module begins stage2, and
callbacks run to completion.

## Tasks and time units

```cpp
namespace core = daveos::core;
using std::chrono_literals::operator""ms;

class Worker : public core::Module<Worker, Event> {
 public:
  static constexpr const char* name() { return "worker"; }

  static constexpr auto tasks() {
    return std::array{DAVEOS_TASK(Worker, Poll)};
  }

  core::Status init(core::InitStage stage) {
    if (stage == core::InitStage::stage1) {
      return schedule<&Worker::Poll>(10ms, core::Mode::repeat);
    }
    return core::Status::ok;
  }

 private:
  void Poll() { /* Do bounded work and return. */ }
};
```

`DAVEOS_TASK` derives the displayed name from the function identifier. Use
`TaskDescriptor<Worker>{"custom name", &Worker::Poll}` when a different name is
useful. The typed `schedule` and `cancel` helpers reject unregistered functions
at compile time. To schedule another module, use
`scheduler().schedule<&Other::Task>(other, delay)`; runtime member-pointer forms
are still available. Helpers may be called after the scheduler binds the module,
including in init, but never from its constructor.

Raw integer delays still mean microseconds. Integral chrono durations make units
explicit: `std::chrono::milliseconds{10}`, or `10ms` with the individual literal
operator imported above. Negative, overflowing, and fractional-microsecond values
return `invalid_argument` without replacing pending work. Floating-point durations
are rejected at compile time. Duration conversion errors take precedence over
lifecycle errors. A zero-delay one-shot is valid; repeat intervals and
interrupt timers must be positive. Valid large delays retain the scheduler's
existing absolute-deadline saturation rule.

## Object-bound timers

From a module, request `timer<&Worker::Expired>(delay)` and cancel with
`cancel_timer<&Worker::Expired>()`. Define `void Expired()` in that module.
From outside it, use `scheduler.timer<&Worker::Expired>(worker, delay)` or
`core::TimerCallback::bind<&Worker::Expired>(worker)` with the ordinary timer API.
Plain functions and noncapturing lambdas remain supported.

A timer is identified by its object and member function. Repeating a request for
that pair replaces the pending timer; another object can use the same function
independently. No global owner pointer is needed. Bound objects are borrowed and
must stay alive until the timer is cancelled/completed and any in-flight callback
returns. Cancellation cannot undo a callback already selected for execution.

Expiry runs in **interrupt context**, just like a plain timer callback. Schedule
a registered task from the callback if further work belongs on the scheduler
thread. Logging remains buffered. No heap allocation or owned closure is involved.

## Command binding

The optional wiring module replaces an application-specific stage2 hook:

```cpp
auto commands = core::make_command_binding<Event>(
    core::CommandSourceList{uart.command_source(), usb.command_source()});
auto modules = core::ModuleList{&commands, &board, &uart, &usb};
auto scheduler = core::make_scheduler<Event>(platform, modules, logger);
core::CommandDispatcher dispatcher(modules, scheduler);

// In the entry point, after all objects exist and before init/run:
commands.connect(dispatcher);
const auto status = scheduler.run();
```

Include `core/command/binding.hpp`. The helper copies source pointers, owns no
transport, and binds them in stage2. All sources and the dispatcher must outlive
its use. Its name is `commands`; reserve that module name. Omitting `connect`
fails initialization with `not_running`. Logging remains independent: omit the
logger argument when it is not needed. Applications can still wire sources
explicitly with `dispatcher.bind_sources()`.

## Failures

Check the status from scheduling and lifecycle operations. Lifecycle and new
convenience calls are `[[nodiscard]]`; an explicit `(void)` documents deliberate
ignoring. Existing raw-microsecond APIs retain their previous annotation behavior.

After an initialization failure, `scheduler.initialization_failure()` returns
`status`, `module`, and `stage`, even with no logger. A null module identifies
registration validation rather than a module hook. The first failure is retained;
initialization is never retried. An attached logger also receives an attempted
error summary before flushing. A failed output transport cannot be relied on to
print it. The STM32 example retains `app::last_status` and
`app::initialization_failure` for inspection in GDB before halting.

## Meson consumption

DaveOS exports `daveos-core`, `daveos-console`, selected platform dependencies, and
`daveos-network` when networking is enabled. Host builds also export the fake
platform. The starter's `dependency(..., fallback: ...)` calls show how to consume
them. Disable DaveOS's own examples/tests for a small consumer build; your own
application tests can use the fake platform without Catch2. Pin the wrap revision
to a tested commit. Device startup, peripheral setup, linker scripts, and board
HAL selection remain the application's responsibility; the shared STM32 console
provides a worked example.
