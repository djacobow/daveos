# Writing an application

Start from [`starters/application`](../starters/application/README.md) for a
separate Meson project with a host executable and a fake-time test. The framework
uses explicit ownership and fixed capacities: module constructors store references
and metadata, all modules finish stage1 before any module begins stage2, and
callbacks run to completion.

## Tasks and time units

```cpp
#include "core/schedule/module.hpp"

namespace core = daveos::core;
using std::chrono_literals::operator""ms;
using Event = daveos::core::NoEvent;

class Worker : public core::Module<Worker, Event> {
 public:
  static constexpr const char* name() { return "worker"; }

  static constexpr auto tasks() {
    return std::array{DAVEOS_PERIODIC(Worker, Poll, 10ms)};
  }

 private:
  void Poll() { /* Do bounded work and return. */ }
};
```

`DAVEOS_PERIODIC` registers and schedules the task: first at 10 ms after normal
dispatch begins, then every 10 ms. Its interval must be a positive, exact integral
chrono duration known at compile time. Defaults are installed before any stage1
hook; explicit schedule/cancel in either stage overrides them. Successful explicit
scheduling/cancellation before init also wins. No callback runs during init.

Use `DAVEOS_TASK` for tasks scheduled dynamically. Both macros derive
the displayed name from the function identifier. Use
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

## Application composition

Include `core/schedule/application.hpp`. For a minimal application:

```cpp
auto application = core::make_application<Event>(platform, core::ModuleList{&worker});
const auto status = application.run();
```

Add a logger, command sources, or both independently:

```cpp
auto modules = core::ModuleList{&board, &uart, &usb};
auto sources = core::CommandSourceList{uart.command_source(), usb.command_source()};
auto application = core::make_application<Event>(platform, modules, logger, sources);
const auto status = application.run();
```

Omit `logger` for commands without logging, or omit `sources` for logging without
commands. Application owns its scheduler and optional dispatcher, borrows the
supplied objects, and needs no wiring module or reserved module name. Sources bind
after both initialization stages succeed, before dispatch starts. They cannot
submit commands during initialization. Construction is passive, including at
file scope; borrowed objects must be constructed before init/run and remain alive
through Application destruction. Application cannot be copied or moved.

Use `application.init()` for explicit initialization or let `run()` do it.
Use `application.scheduler()` for scheduling and diagnostics; lifecycle calls
must go through Application. Customize storage with a named compile-time value:

```cpp
constexpr core::Capacities capacity{
    .events = 8, .timers = 4, .line = 128, .arguments = 8};
auto app = core::make_application<Event, capacity>(platform, modules, sources);
```

Omitted fields retain their defaults: 32 events, 16 timers, 256 line bytes,
and 8 tokens including prefix and command. For a program without events,
`core::Module<Worker>` and `core::make_application(platform, modules)` default
to `core::NoEvent`. For capacity-only tuning, use
`core::make_application<core::Capacities{.events = 64}>(platform, modules)`.
This form defaults to `core::NoEvent`; the Event-first form remains available.
Logger arguments must satisfy `core::LoggerFor<Logger, Platform>`;
logging and command sources remain independent.

Custom applications can still assemble `make_scheduler`, `CommandDispatcher`,
and `bind_sources` directly. The existing CommandBinding helper provides stage2
binding when using those explicit pieces.

## Failures

Check the status from scheduling and lifecycle operations. Lifecycle and new
convenience calls are `[[nodiscard]]`; an explicit `(void)` documents deliberate
ignoring. Existing raw-microsecond APIs retain their previous annotation behavior.

After an initialization failure, `application.initialization_failure()` returns
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
application tests can use the fake platform without Catch2. Starter wrap revisions
pin the tested variant-event API commit; keep upgrades pinned too. For a device
application, use [the STM32 starter](../starters/stm32/README.md):
its board dependency supplies CPU/ABI flags, startup, linker, and HAL settings.
Custom boards supply their own equivalents.

For the guided progression, start with [hello](01-hello.md). Use
[host::stdout_subscriber and host::run](02-logging-and-commands.md) for standard
host output and exit handling, and [Console](03-hardware-console.md) for reusable
STM32 transport registration.

## Events with payloads

Use an application-defined `std::variant` of small payload structs. A module's
`events()` tuple registers `DAVEOS_EVENT(Module, Handler)` entries; the handler's
`const Payload&` parameter selects the alternative it receives. Other alternatives
are ignored. `scheduler().post(payload, this)` copies the value and excludes the
sender. See [payload events](reference.md#payload-events) for a complete example,
validation rules, and the explicit `std::visit` alternative.

## Command-limit migration

The default token capacity changed from 16 to 8, counting the module and command
names. This affects raw `CommandArguments` handlers too: a command with ten data
arguments now returns `too_many_arguments` before calling its handler. For an
event-free application that needs ten raw arguments, raise capacity explicitly:

```cpp
auto app = core::make_application<core::Capacities{.arguments = 12}>(
    platform, modules, sources);
```

With application events, use `make_application<Event, capacity>(...)` (or
`make_application<capacity, Event>(...)`). Raising token capacity does not raise
the six-parameter limit for typed handlers. For 64-bit integer or `double`
parameters, omit `.min()`, `.max()`, and `.range()` and check bounds in the handler,
or change the bounded parameter to a 32-bit integer or `float`.
