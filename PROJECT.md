# DaveOS

## Purpose

Build a simple, non-preemptive task scheduler suitable for embedded projects.

The shared embedded console runs on NUCLEO-H563ZI and NUCLEO-H755ZI-Q, with
LED/button commands, UART and USB CDC transports, and optional lwIP Ethernet/TCP.
Host and fake-time examples exercise the same scheduler independently of hardware.
Longer-term applications may include multiple UARTs, CAN buses, and additional
network listeners beyond the current single-client TCP console.

## Language and Tools

* Implement in C++20.
* No exceptions.
* No dynamic memory allocation in the core or embedded code, except allocation
  from fixed preallocated pools inside optional networking; no runtime heap.
* The basic host platform should avoid allocation where practical, but may allocate
  when necessary. Test infrastructure may allocate freely.
* Standard library facilities are allowed subject to these allocation rules.
* Use `constexpr` where possible.
* Prefer CRTP and static dispatch to virtual methods for modules and platforms.

## Namespaces

| Namespace | Contents |
| --- | --- |
| `daveos::core` | Scheduler, module interface, task descriptors, events, timers, queues, optional logging, command descriptors and dispatch, name matching, status enums, and the platform contract. |
| `daveos::platform::host` | Real-time host platform and simulated interrupts. |
| `daveos::platform::stm32h5` | STM32H5 platform implementation. |
| `daveos::platform::stm32h7` | STM32H7 platform implementation (H755 M7). |
| `daveos::platform::fake` | Fake clock, timer, and sleep implementation. |
| `daveos::console` | Shared line collection, display, buffered output, and CRTP console module. |
| `daveos::net` | Optional standalone lwIP service, TCP server, and thin module adapter. |
| `daveos::net::stm32` | Shared H5/H7 Ethernet driver and board network configuration. |

The core platform contract belongs in `daveos::core`; concrete implementations
belong under `daveos::platform`. Application modules and event enums use
application-owned namespaces.

## Design

DaveOS has a hardware-independent core with platform support injected at
construction. The core depends only on permitted C++ standard library facilities.
Platform implementations may depend on an OS or other libraries.

The core provides a scheduler with one or more modules, an optional logging
service, and an independent command dispatcher. Applications may use logging,
commands, both, or neither.

### Execution model

Module callbacks run to completion; the scheduler does not preempt them.
Long-running tasks must implement their own state machines, splitting work across
callback invocations so that other tasks can run.

A limited yield mechanism may be considered later, but is outside the initial scope.

Interrupt handlers may post events and schedule tasks. These operations hand work
off to the scheduler; they do not execute module callbacks in interrupt context.
Scheduled callbacks and event callbacks execute in the scheduler's execution context.
Shared scheduler state is protected by short platform-provided critical sections.

### Module

A module is a class with zero or more schedulable member functions. Modules may
provide commands, handle events, or participate in initialization without declaring
any tasks. The default task and command descriptor arrays are empty.

A module also has:

* A nonempty `static constexpr const char* name()` accessor.
* A `can_sleep()` callback that indicates whether the module permits system sleep.
* An `on_event()` callback that receives an application-defined event enum value.
* An `init(InitStage stage)` callback used at startup.
* A reference or pointer to the module-facing scheduler interface.

Each module may override `tasks()` to expose a constexpr array of task descriptors
and `commands()` to expose command descriptors (see Command System below).
Each task descriptor pairs a human-readable string name with its schedulable member
function. The array defines the task callbacks and their names; its size provides
the task count without a separate count accessor. Statistics tables identify tasks
by module name and task name.
Module names are nonempty static constexpr metadata and must be unique ignoring
ASCII case within a scheduler, checked at compile time. Task names must be unique
within their module; different modules may use the same task names.

### Module initialization

The public module API defines `enum class InitStage` with values `stage1` and
`stage2`. During successful startup, each module's `init(InitStage stage)` callback
is called once per stage. All modules complete `stage1` before any receives `stage2`.

* `stage1`: independent initialization, including preparing handles that other
  modules may need.
* `stage2`: initialization that requires handles from other modules.

Module initialization callbacks return the common scheduler status enum, including
success and a general initialization-failed value. Modules log application-specific
failure details separately. Initialization stops at the first failure; remaining
initialization callbacks are not invoked.

The application wires references and handles between modules. The scheduler does
not provide module lookup or dependency resolution.

### Scheduler

The scheduler registers its modules and receives the platform at construction.
The application may also attach its logger by reference. Use
`make_scheduler<Event, Events, Timers>(platform, modules)` without a logger, or
pass the logger as the third argument. Event and timer capacities default to
32 and 16; log capacities belong to the logger.
The application constructs and owns the module instances, passing their pointers
in a typed `ModuleList`, for example `ModuleList{&fooMod, &barMod, &bazMod}`.
The list preserves each module's concrete type so module counts and task descriptor
array sizes can be derived at compile time. It holds pointers rather than owning
the modules; module instances must remain alive for the scheduler's lifetime.

Modules use the CRTP base `Module<Derived, Event>`. Platforms similarly use a
CRTP base and are statically bound to the concrete scheduler.

The scheduler provides `SchedulerInterface<Event>`, a small module-facing
interface parameterized only by the application's event enum. It exposes
scheduling and cancellation, event posting,
timers, and logging without exposing the concrete scheduler's module-list types or
storage capacities. Modules depend on this interface rather than the full scheduler
template specialization, so they can be reused across scheduler configurations.
This interface is an allocation-free reference with a function-pointer operation
table, not a virtual base. Heterogeneous task and subscriber callbacks likewise
use stored function pointers where indirect dispatch is required.

Constructors store references and static metadata without accessing prerequisites
or initializing other objects. During `init()`, after validation and before any
`stage1` callback, the scheduler binds this interface into every module. Module
constructors must not access it. All modules complete `stage1` before any module
enters `stage2`; independent setup belongs in stage1, and setup that uses another
module's initialized state belongs in stage2.

Its operations are:

* `init()`: perform both module initialization stages.
* `run()`: initialize if needed, then dispatch work until stopped or startup fails.
* `schedule(...)`: set a callback's delay/interval and one-shot/repeat mode.
* `cancel(...)`: remove the schedule for a module instance and member function.
* `stop()`: request exit from `run()` where stopping is supported.

### Scheduler lifecycle

The application may call scheduler `init()` explicitly. The scheduler tracks
initialization state, and `run()` calls `init()` automatically only if initialization
has not yet been attempted. Both initialization stages finish before scheduled tasks or
events are dispatched. Modules may schedule tasks and post events during
initialization; dispatch waits until initialization completes.
Calling scheduler `init()` again after initialization completes returns an error
without invoking module initialization callbacks or changing scheduler state.
If a module initialization callback fails, scheduler `init()` flushes buffered
logs to subscribers before returning that error. This failure-path flush runs
without dispatching scheduled tasks or events, even if they are already due.
If initialization fails during `run()`, `run()` returns the error without starting
normal task or event dispatch.
Initialization failure is terminal for that scheduler instance. Subsequent `init()`
or `run()` calls return an error without retrying initialization or dispatching work.
Discard pending task schedules and queued events on initialization failure; none
of that work is dispatched. Buffered logs still follow the failure-path flush rule.
`initialization_failure()` retains the first status, module name, and stage even
without logging; a null module denotes registration validation, where stage is
irrelevant. With logging enabled, the scheduler attempts to enqueue an error
summary before flushing; delivery still depends on functioning subscribers and
buffer/filter limits. The STM32 application retains platform/run status and the
failure snapshot for debugger inspection before halting. Lifecycle init/run and
new checked convenience operations are `[[nodiscard]]`.

The host supports stopping so tests can return from `run()`. A stop request does
not interrupt an executing callback; an in-progress event broadcast completes
before `run()` returns. Embedded targets may treat `stop()` as a no-op and run
indefinitely.

Calling `stop()` before `run()` returns `not_running` without changing state or
preventing a subsequent run.
`run()` is single-use per scheduler instance on every platform. A stopped scheduler
cannot be restarted: another `run()` call returns an error without restarting
dispatch or initialization.

On host shutdown, disarm the platform timer and discard pending DaveOS timers,
including the scheduler's reserved timer. Complete any timer callback already in
progress and prevent further timer callbacks before returning from `run()`.
Flush buffered logs to subscribers after timer shutdown and before `run()` returns,
without dispatching further tasks or events.
Control returns to the application with DaveOS execution stopped; this does not
undo application state changes made while it ran.

### Task scheduling

A schedule is identified by the module instance and member function. Scheduling
that same function on the same module again replaces its existing schedule,
including its timing and one-shot/repeat setting. It does not create an independent
schedule. Different functions on a module, or the same function on different module
instances, have independent schedules.

Before normal dispatch starts, scheduling requests retain their requested delays.
This includes requests from interrupt handlers during initialization.
When `run()` starts normal dispatch after successful initialization, those delays
are measured from that common start time. Initialization time and any wait between
explicit `init()` and `run()` do not create task lateness or overdue iterations.

Once normal dispatch has started, scheduling delays are measured from when
`schedule()` is called, including calls from task callbacks and interrupt handlers.
A zero delay is valid for a one-shot and makes the task immediately eligible for
scheduler dispatch; it does not invoke
the callback inline. Repeating schedules require a strictly positive interval;
requests with a zero repeat interval are rejected.
Repeating schedules use that same interval for the first execution delay and all
subsequent iterations. There is no separate initial-delay parameter.

Cancellation removes pending iterations of that schedule. A callback already
executing still runs to completion.
Cancelling a task with no active schedule returns an error without changing state.
Task cancellation from interrupt handlers is deliberately outside the initial API
scope because it is not needed for the initial applications. This does not restrict
the agreed interrupt support for task scheduling and replacement.

A task may reschedule or cancel itself during its callback. That explicit action
takes precedence over automatic repetition; callback completion must not restore
the previous schedule or overwrite the replacement.

If a task repeatedly takes too long, the scheduler continues running it on a
best-effort basis to keep up while recording the timing problems.
A missed deadline means that the iteration runs late; it is not skipped or combined
with another iteration. Repeating tasks retain their scheduled cadence: each next
due time advances from the previous scheduled due time by the repeat interval,
not from the actual completion time. Every overdue iteration remains due until run.
There is no catch-up cap or additional fairness policy. For example, ten overdue
iterations remain ten executions unless their schedule is explicitly replaced or
cancelled. Other tasks and events interleave according to their due times; they do
not wait for the entire backlog if their due times precede remaining iterations.
Sustained overload may keep later-due work waiting and prevent idle log dispatch.

### Dispatch ordering

After a task callback returns or an event broadcast completes, the scheduler
selects the earliest due item across scheduled tasks and events. This also applies
when multiple task iterations are overdue.

Event delivery uses the event's posting time as its due time. Events and scheduled
task iterations follow the same earliest-due-time-first ordering.
Ordering is unspecified for equal due times, whether between tasks, between
events, or between a task and an event.
Once an event is selected, it is delivered to all recipient modules before the
scheduler selects another task iteration or event. Events posted during delivery
are queued for later dispatch.
The initial implementation visits recipients in module registration order, but
reception order is not guaranteed by the API. Modules must not depend on it.

### Task timing statistics

For each task (module instance and schedulable callback), track execution count,
late-start count, maximum start lateness, and minimum and maximum execution
duration. Retain total execution duration so average execution duration can be
provided from the total and execution count.

Every measured start time later than the scheduled due time counts as late;
there is no tolerance. Execution duration is measured from callback entry to
return using the platform's microsecond clock.

Provide an API to obtain a snapshot of per-task statistics and diagnostic counters,
and an explicit operation to reset them. Also provide a helper that logs a readable
table from a snapshot, identifying tasks by module and task name and showing
execution counts, late-start counts, maximum lateness, and minimum, maximum, and
average execution duration. Include event and timer overflow counters. Logging
counters (dropped and truncated messages) belong to the logger and have their own
snapshot/reset API.

### Error reporting

Define a common `enum class` for errors the scheduler can report or encounter.
Use explicit status returns rather than exceptions, with a success value for
operations that complete normally. Distinguish error conditions such as repeated
initialization, module initialization failure, invalid repeat interval, event
queue overflow, timer overflow, and `not_running`. The shared type is
`daveos::core::Status`, declared in `core/platform/platform.hpp`; commands use it too.
For scheduler operations, `not_running` reports unavailable execution, including
pre-run `stop()` and valid timer requests during init, or work submitted after
shutdown or terminal initialization failure.
An unbound command source or unconnected `CommandBinding` also uses `not_running`
to report unavailable dispatch. These are explicit failures, not silently
accepted operations.
Error counters retain aggregate diagnostics where specified; they do not replace
the operation's explicit failure result.

### Idle and sleep

When no task or event is due and no logs await delivery, the scheduler may sleep
if the platform provides sleep support and every module's `can_sleep()` returns
true. The requested sleep duration is limited by the earlier of the next scheduled
task and the next pending timer. If the platform actually remains asleep past a
timer's due time despite that bounded request, behavior is undefined; timer delivery
and recovery are not guaranteed. This covers a platform sleep overrun, not normal
timer dispatch. Plan sleep to end no later than the next timer due time rather
than relying on that timer to wake the part.
Interrupts can wake the scheduler early so it can re-evaluate pending work.
If sleep is unavailable or any module disallows it, the scheduler waits without
sleeping, remaining responsive to newly posted events and scheduling requests.
For a timed wait while awake, it uses a timer callback to signal when work is due.
Wait notification mechanics are internal to the platform; no separate public
`wait()`/`wake()` API is required. Awake waits respond to timer expiry and incoming
interrupts, while sleep follows the platform's timed and interrupt-wakeup contract.

If no tasks or timers are pending, no events are pending, and no logs await
delivery, the scheduler sleeps indefinitely until a waking interrupt arrives,
provided the same sleep conditions are met. Otherwise, it continues waiting
without sleeping.

### Storage and capacity

Module registration storage and task schedule storage are sized at compile time from the
registered modules and their schedulable callbacks. Each module instance has one
schedule slot per schedulable callback, so scheduling a declared callback does not
exhaust task storage. Task counts are derived from the module task descriptor arrays.

Event queue capacity is a scheduler template parameter defaulting to 32 events.
The scheduler contains a fixed-size backing array sized by that parameter, with
no dynamic allocation.

If an event post exceeds available capacity, it returns an explicit failure to the
caller and increments an overflow counter. Already accepted events are preserved.
This applies to posts from both module callbacks and interrupt handlers. If the
interrupt handoff design introduces a bounded scheduling-request buffer, exhaustion
must likewise fail explicitly and be recorded.

## Platform interface

The injected platform supplies:

* A monotonic clock returning unsigned 64-bit microseconds.
* Exactly one asynchronous timer taking a delay and callback. Arming it returns without
  waiting; execution may continue while the timer waits, and the callback runs
  after the delay.
* Optional blocking sleep taking a delay. Scheduler execution is suspended during
  sleep and resumes at the call site when the delay expires. Sleep also supports
  indefinite waiting and early interrupt wakeup as described in the idle rules.
* Critical-section entry and exit primitives for shared scheduler state.

On embedded targets, critical sections temporarily mask interrupts and restore
the previous interrupt state on exit. Keep these sections short. Module callbacks
and logging subscribers execute outside scheduler critical sections.

Host critical sections are mutex-backed. Scheduler execution and simulated
interrupt handlers use the same protection for shared state. Interrupt dispatch
coordinates with this protection so a new handler does not start while the
scheduler holds a critical section. Handlers already running may execute
concurrently with module callbacks, but their protected operations remain mutually
exclusive. Nested critical sections must preserve protection until the outermost
section exits.

The platform may also provide an optional mutex facility.

Timer and sleep are distinct operations: arming a timer does not suspend execution,
while calling sleep does.

## DaveOS timers

DaveOS provides multiple simultaneous logical timers over the single platform
timer. Each logical timer takes a delay and callback. The timer layer tracks
pending due times and arms the platform timer for the earliest one. When the
platform timer fires, the layer processes due timers, finds the next due time,
and rearms the platform timer. Adding an earlier timer must update that arm.

The public `TimerCallback` is a non-owning, allocation-free value accepting a
plain `void (*)()` function (including a noncapturing lambda), or an object and
compile-time selected `void` member callback with no arguments. Plain functions
retain function-pointer identity. Bound callbacks use object-plus-member identity;
binding through base/derived references is normalized to the declaring class.
Different objects can independently use the same member callback. The object must
outlive pending work and any already-executing callback. No owned callable or
arbitrary argument payload is introduced. Callback storage is three pointers.
`TimerCallback::bind<&Type::Function>(object)` creates an explicit bound callback;
the scheduler interface and module provide typed timer/cancellation helpers.

DaveOS timer requests require a strictly positive delay. A zero-delay request
returns an error without creating a timer or modifying an existing timer.

Timer requests with valid delay arguments made before the scheduler starts normal
dispatch return `not_running` and otherwise do nothing, including requests during initialization.
They do not reserve a slot, arm the platform timer, or carry forward into execution.
Applications needing timers
during initialization must arrange them outside the DaveOS timer API.
This intentionally differs from task scheduling: tasks may be registered for later
dispatch during startup, while DaveOS interrupt-time timers are a running-scheduler
facility and do not provide initialization-time timing services.

Requesting a timer for a callback that already has a pending timer replaces that
timer's due time rather than creating another timer. Applications needing separate
timers for the same underlying behavior on one object use distinct wrapper
functions; separate objects already have distinct callback identities.

Provide `cancel_timer(callback)` to remove the pending timer identified by that
callback and release its slot. Update the platform timer's arm if the earliest
pending timer changes. Cancellation does not interrupt a callback already executing.
If no pending timer matches the callback, cancellation returns an error without
changing timer state.
Timer cancellation is allowed from interrupt handlers, including another timer
callback, so interrupt-time timer activity can cancel pending timers directly.
This is separate from task cancellation, which remains outside the interrupt API.

A timer callback may rearm itself by requesting another timer. The expired timer's
slot is released before invoking its callback, making that slot available for
rearming even when all other application timer slots are occupied.

Scheduler timed waits share this single platform timer through the timer layer.
DaveOS timer callbacks execute in interrupt context (simulated interrupt context
on the host). A timer callback that needs task-context execution schedules a task
for later dispatch by the scheduler.
Application timer capacity is a template parameter defaulting to 16. Timer storage
is fixed, with one additional slot reserved for the scheduler's timed wait so
application timers cannot consume it. Pending timer due times limit scheduler
sleep duration as specified in the idle rules.
When all application timer slots are occupied, a request requiring an additional
slot returns an explicit failure and increments a timer-overflow counter.
Replacing an existing callback's pending timer reuses its slot. Existing timers are
preserved, and the scheduler's reserved slot remains unavailable to applications.

## Queues

Provide a templated queue class and a thread-safe templated queue class.
Provide `push()`, `pop()`, `peek()`, `clear()`, `size()`, `capacity()`, `empty()`,
and `full()`. `peek()` copies the front item without removing it and reports
failure when empty. `clear()` removes all queued items. Keep the API concise and
convenient, with explicit failure results where an operation can fail.

The thread-safe queue uses the platform mutex when it provides interrupt-safe,
nonblocking `try_lock()` and `unlock()` operations. If no suitable mutex is
available, it falls back to platform critical sections.
The queue must support nonblocking push and pop from interrupt handlers, returning
explicit failure when full (push), empty (pop), or unable to acquire the required
protection immediately. Failed operations leave the queue unchanged.
Each queue uses the same synchronization mechanism for interrupt and
non-interrupt callers. Interrupt operations fail immediately if the mutex is busy.
Operations accessing mutable queue state use the queue's synchronization mechanism.
Size and state queries are snapshots, not guarantees that a subsequent push or pop
will succeed.

## Events

Any module can post an event to the scheduler. Events carry only an
application-defined enum value, with no payload in the initial version.
Applications define their events without modifying DaveOS.
The application event enum type is a scheduler template parameter. Event posting
and module event callbacks use that enum type directly, preserving type checking.
Event posts may identify a sender module, including posts from interrupt handlers.
When the sender is known, it is excluded from delivery. Without an identified
sender, the event is delivered to every module. Sender identity is internal
delivery metadata; the event callback still receives only the enum value.

## Logging

Logging is accessible to all modules through an optional service.

### Ownership and optional builds

Logging is an application-owned optional service, independent of commands and
outside `ModuleList`. Construct the logger before the scheduler and pass it by
reference when attaching it; it and its subscribers must outlive the scheduler.
`make_logger<Records, MessageBytes>(platform, subscribers)` deduces the platform
and subscriber types. The logger and scheduler must use the same platform.
Different platform types are rejected at compile time; different instances of the same type make scheduler `init()` return
`Status::invalid_argument` before any module initialization callback. This failure
is terminal and follows the normal initialization-failure cleanup and log flush.
There is no platform check when logging is compiled out.
The scheduler owns no logging buffers, subscriber storage, filtering state, or
logging counters. Logger construction makes logging available before module
initialization. Calls through `scheduler().log(...)` remain forwarding conveniences.

A build-wide Meson boolean `logging`, enabled by default, controls logging support.
With logging disabled, severity macros return `Status::ok` without evaluating
any arguments, and logging buffers, formatting, and scheduler logging hooks are
omitted. Direct log calls become successful no-ops but still evaluate arguments
according to normal C++ function-call rules. With logging enabled but no logger
attached, log calls also return `Status::ok` and discard output. Commands and
logging support all four combinations independently.

The logger owns its severity threshold (`minimum()`), diagnostic snapshot
(`counters()`), and counter reset (`reset()`).
Scheduler snapshots and resets cover only task, event, and timer statistics;
the scheduler's table helper uses the optional logging API. A scheduler without
an attached logger has no log-draining work. An attached logger delivers one
record during idle dispatch, after which the scheduler checks due tasks/events
again. Pending records prevent sleep. Successful enqueue, including from interrupt
context, notifies the platform so idle delivery cannot be stranded by a race with
sleep entry. Initialization failure and shutdown flush remaining records.

### Record and delivery contract

- Initially use printf-style formatting during the logging call, capturing argument
  values into fixed message storage before returning, including in interrupt context.
  Formatting must obey the allocation and interrupt-safety requirements.
- Logging is implicitly line-oriented: format strings need no trailing newline.
  Output subscribers append the newline automatically when emitting each record.
- A MISRA-friendly alternative formatting interface is a future enhancement,
  outside the initial implementation scope.
- Levels: `debug`, `info`, `warning`, `error`, and `fatal`.
- a configurable minimum severity filters out lower-severity messages before
  they enter the log buffer; it defaults to `info` and can be changed at runtime
- `fatal` is only a logging severity in the initial version; it does not
  automatically halt, reset, or stop the scheduler
- Without subscribers, messages are discarded. A host subscriber could print to
  standard output; an embedded subscriber could write to a UART.
- Subscribers are supplied in a list at logger construction time and are available for
  initialization diagnostics. Fixed subscriber storage is sized at compile time
  from that list; no separate subscriber-capacity setting is needed.
- logging is buffered and may be called from module callbacks and interrupt
  handlers; logging calls enqueue records without invoking subscribers
- subscribers receive buffered records later in the scheduler's execution
  context, outside interrupt handlers
- Each subscriber-facing record includes timestamp, severity, message text,
  module name, and task name. Attribution must be captured when the log call
  occurs and retained for deferred delivery. Calls outside a module task use
  descriptive context labels such as `core.init` or `core.interrupt`. Interrupt logs
  must identify interrupt context rather than inherit the interrupted task's name.
- During normal operation, dispatch logs only when no tasks or events are due,
  checking for due work between records. Initialization failure and normal host
  shutdown flush buffered logs before returning, as specified in the lifecycle rules.
- buffered records must retain the message data needed for later delivery
  without dynamic allocation
- log buffer capacity and per-record message storage size are template parameters,
  defaulting to 32 records and 128 bytes of message storage per record. The storage
  includes the null terminator, allowing 127 bytes of text by default. The newline
  appended by the output subscriber does not consume message storage.
- each record captures the platform's monotonic microsecond timestamp when the
  logging call occurs; subscribers receive that original timestamp for later output
- when the log buffer is full, reject the new message and increment a dropped-log
  counter, preserving records already queued
- messages exceeding the per-record storage size are truncated, preserving the
  beginning of the message, and increment a truncated-message counter

## Supported platforms

The implementation provides real-time host and fake-time adapters, an STM32H563
adapter and an STM32H755 M7 adapter with one shared, board-selected console.
UART, USB CDC, and optional TCP transports register independently.
STM32CubeH5 and STM32CubeH7 are pinned as submodules; no board BSP is used.
The H755 M4 completes CubeMX boot synchronization, disables SysTick, and sleeps
in a WFI loop. It does not run DaveOS or access M7-owned peripherals.
The core remains hardware-independent; hardware validation is tracked in TODO.md.

* Host: clang++.
* ARM: arm-none-eabi GCC and the pinned STM32H5/H7 HALs.
  The initial MCU target is STM32H563.

Host support provides both:

* A fake clock and timer for fast, deterministic unit tests without real-time waits.
  Tests can advance time manually or enable automatic advancement to the next due
  task or timer when the scheduler would otherwise wait.
  Advancing time delivers due timer callbacks synchronously before the advance
  operation returns. Callbacks run in simulated interrupt context and follow the
  same interrupt serialization and critical-section rules. Fake time requires no
  dedicated timer thread.
  Fake-time sleep uses the same advancement modes as awake waiting. In automatic
  mode it advances to the next wake deadline. In manual mode it waits for the test
  to advance time or trigger an interrupt. With no deadline, it waits for an
  explicit simulated wakeup rather than advancing time indefinitely.
* A real-time platform using a monotonic host clock, such as
  `std::chrono::steady_clock`, for tests that run in real time. Integration tests
  may interact with other processes through sockets or similar host facilities.

Both use the same scheduler core through the injected platform interface.

The real-time host platform implements its asynchronous timer with a dedicated
timer thread. The thread waits for the armed expiry and invokes the timer callback
through the serialized simulated-interrupt mechanism, respecting host critical
sections. Rearming or shutdown wakes the thread to update or end its wait.

Host threads may trigger simulated interrupts through the host platform. The
simulated interrupt handlers may then use interrupt-supported operations, including
scheduling tasks and posting events. Direct scheduler access from arbitrary host
threads is not part of the supported concurrency model. Simulated interrupts must
respect platform critical sections and be able to wake an idle scheduler.
Simulated interrupt handlers run concurrently with module callbacks, allowing host
tests to exercise interrupts arriving during callback execution.
Host simulated interrupt handlers are serialized with one another: only one runs
at a time. Nested interrupt execution and interrupt priorities are outside the
initial host simulation scope.

## Build system

Use Meson. Python helpers currently use only the standard library; use uv if
Python package dependencies are introduced.
Build configurations live under `build/` (for example `build/host`, `build/fake`,
`build/asan`, `build/arm`, and `build/h755`). All generated intermediates and caches belong under
that ignored directory and can be removed and regenerated.
Provide `format`, `format-check`, and `lint` build targets using clang-format and
cppcheck.
STM32 examples provide explicit `flash` (CubeProgrammer), `flash-openocd`, and
`flash-plan` targets. Programming builds and verifies the firmware before resetting;
H755 includes both core images. Tool paths and ST-LINK serial are configurable.

## Style

* Emphasize simplicity and concision in design and implementation.
* Be DRY (Don't Repeat Yourself): keep each piece of logic or knowledge in one place.
* Use Google C++ style with the repository's clang-format overrides: blank lines
  between function/class definitions, braces on control-flow bodies, and indented
  namespace contents.
* Use short namespace aliases and qualified names instead of `using namespace`.
* Follow the fixed-width integer and file-naming rules under Source organization.
* Allow short variable names if their physical scope, from first to last textual
  appearance, is less than 15 lines.
* Do not repeat the class name in member names.

## Remaining platform validation

The H563 and H755 M7 examples share the board console implementation: LED
control, button input, scheduler/DMA statistics, reset, UART echo, and formatted
logging. USART3 runs at 1 Mb/s on PD8/PD9 with interrupt-fed input and two 4 KiB
ping-pong TX DMA buffers. H563 uses GPDMA1 Channel 0 and normal SRAM; H755 uses
DMA1 Stream 0 and AXI SRAM. Full buffers drop whole output frames and expose
counters. Both use internal HSI, TIM2 at 1 MHz, and shallow WFI sleep.
The shared STM32 board console includes `board timer <microseconds>`: a positive
unsigned decimal delay starts a one-shot DaveOS timer; its interrupt-time callback
logs `Timer fired` for deferred delivery. Reissuing replaces the pending timer.

H563 LEDs are PB0/PF4/PG4; H755 LEDs are PB0/PE1/PB14. Both read PC13.
H563's CPU runs at nominal 250 MHz, H755 M7 at 400 MHz. H563 hardware checks
passed for boot, UART bursts and TX DMA, USB/TCP commands, timer completion,
Ethernet, and software-reset recovery. Earlier physical checks also confirmed
all LEDs, button press/release, USB-C orientations, and cable reconnection.
Injected UART/DMA errors, precision timing, and prolonged sleep/backpressure
stress remain outstanding. H755 additionally boots M4 into sleep; its earlier
hardware results predate the static-storage, FIFO, board/composition, and
convenience-API changes. Current H755 coverage is build-only, including Application composition. H563
smoke tests also passed after the Application migration: UART/USB/TCP commands,
timers, statistics, button reads, LED acknowledgements, large-packet ping, TCP
reconnect, and software-reset recovery; no new physical LED/button confirmation. See TODO.md for
remaining work. Shared code changes require both board selections to build.

The application may call `platform.reset()` directly, independently of the
scheduler. STM32 H5/H7 request an immediate system reset without returning
(both cores on H755); no initialization, shutdown, or log drain is required.
Host/fake return `Status::unsupported` without changing state. Each STM32 board
module exposes this as `board reset` with no arguments.

## Implementation review checklist

These checks follow from the agreed behavior; they do not introduce additional APIs.

* Verify stage ordering, automatic initialization, terminal failure, repeated-init
  errors, and failure-path log delivery.
* Verify earliest-first dispatch, retained overdue iterations, replacement,
  cancellation, self-rescheduling, zero-delay validation, and per-task statistics.
* Verify sender exclusion, complete event broadcasts, fixed capacity, and overflow
  reporting without depending on unspecified equal-time or recipient ordering.
* Verify timestamped logs, severity filtering, truncation, overflow, and dispatch
  behind due tasks/events.
* Verify queue failures preserve contents and all callers use consistent protection.
* Exercise concurrent host interrupts, restored critical-section state, and wakeup
  arriving between the idle check and sleep entry so work cannot be stranded.
* Verify fake-time tests and real-time host tests through the same core interface.
* Verify timer replacement, cancellation, self-rearming, positive-delay validation,
  pre-run errors, capacity limits, and the reserved scheduler slot.
* Verify sleep is bounded by pending task and timer due times, and shutdown ends
  timer activity and flushes logs before returning.
* Verify compile-time module/command metadata validation, case-insensitive exact
  and unique-prefix matching, quoting, input limits, help, and handler statuses.
* Verify handler logging context and argument views survive nested-dispatch
  rejection, and that command-only modules require no task slots.
* Exercise all four combinations of commands and logging. Disabled macros must
  skip argument evaluation; disabled logger instances must have no buffer storage.
* Verify logger counters reset independently of scheduler statistics, interrupt
  enqueue notifies idle dispatch, and pending records prevent sleep.
* Verify console EOF does not stop the scheduler; `console exit` does.
* Verify exact chrono conversion, invalid-delay preservation, compile-time task
  selection, bound-timer identity/cancellation, and retained initialization errors.
* Verify the optional command-binding module and separate consumer starter build.

Public headers define concrete C++ signatures and status values. Changes to those
interfaces must keep this specification and the application examples consistent.

## Command System

### Typed argument adapters

Add typed handler registration in the command layer, independent of scheduler
internals. Keep raw `CommandArguments` handlers available for unusual syntax.
The dispatcher remains the single tokenizer; adapters consume its argument views
without allocating or splitting the line again.

Infer parameter types and required/optional counts from member-function pointers
using C++20 templates. Validate the complete argument list before invoking the
handler, which returns `Status`. Reject extra arguments, missing required
arguments, conversion failures, and values outside declared bounds. Trailing
`std::optional<T>` parameters accept omitted arguments as `std::nullopt`; invalid
supplied values are errors. Handlers choose defaults with `value_or()`; C++
default parameter values are not inferred.

Provide parsers for integers, floating-point values, and booleans. Numeric
conversion consumes the entire argument and rejects overflow. Integer and float
metadata can specify optional inclusive minimum/maximum bounds through
`arg("name").min(value)`, `.max(value)`, or `.range(minimum, maximum)`.
Reject NaN and infinity. Strict boolean parsing accepts `true` and `false`.
Opt-in `.friendly()` accepts the pairs `true/false`, `1/0`, `on/off`, `yes/no`,
`enable/disable`, `high/low`, and `set/clear`; unknown values are errors.

Keep parsing and compile-time validation in a directly usable
`command<&Board::Timer>("timer", "Start a timer", metadata...)` factory.
Use a thin macro as the normal registration shorthand, capturing the C++ handler
name for logging while keeping the public command name explicit:

```cpp
Status Timer(std::uint32_t microseconds);
// Inside the module's constexpr commands() array:
DAVEOS_COMMAND(Board, Timer, "timer", "Start a timer",
               arg("microseconds").range(1u, 60'000'000u))
```

The macro order is module type, handler identifier, public name, help, then
argument declarations. Do not use macros to declare handler functions or
their parameters. Factory results share a descriptor type within each module,
allowing different handler signatures in one array. Validate metadata count,
type-compatible bounds/policies, and trailing optional parameters at compile
time. Argument names support useful diagnostics and generated help showing
`<required>` and `[optional]` parameters. Preserve actual module/C++ handler
logging context. Direct factory calls derive the function label from GCC/Clang
compiler signatures; the macro captures the identifier explicitly.

Integer syntax is decimal, with optional `0x`/`0X` hexadecimal and `0b`/`0B`
binary prefixes. Leading zeros remain decimal. A single leading sign is allowed;
unsigned types reject minus signs. Floats (`float` and `double`) accept decimal
and scientific notation, including one leading sign; reject nonfinite results,
underflow/overflow, hexadecimal forms, whitespace, and trailing text. Conversion
uses `std::from_chars` and is locale independent. Bounds are validated and
converted to the parameter type at compile time; integer bounds must be integral
and representable. Friendly boolean aliases are ASCII case-insensitive; strict
booleans are exactly lowercase `true` and `false`. Borrowed `std::string_view`
parameters are also supported, including optional text.

Each homogeneous descriptor owns fixed metadata for up to 16 typed parameters;
this is separate from the dispatcher's configurable token limit (16 by default,
including module and command). Descriptors live in static constexpr storage in
the dispatcher, not on the dispatch stack. No handler runs after an adapter
failure. Report `invalid_argument` and log the parameter name/type or expected
count through the ordinary optional logger. Help lists required/optional names
and types below each command. Raw handlers retain their own validation.
The application handlers use typed parameters: board LED takes a bounded 1–3
index and an unconverted action string (`on`, `off`, or `toggle`), and network
status/host exit take no arguments. Host echo deliberately remains a raw handler
because it accepts an arbitrary number of tokens.

Test the public adapters across signed/unsigned 8/16/32/64-bit integers, float,
double, strict/friendly booleans, and unconverted string views, both required and
optional. Cover numeric limits, all 8-bit values, malformed tokens, inclusive
bounds and adjacent floating-point values, all boolean aliases/case variants,
quoted/escaped/empty text, omitted versus explicitly empty strings, chains of
optional parameters, and entirely optional handlers. Rejected input must not
invoke the handler or retain partially parsed values. Compile checks cover
invalid signatures, policies, bounds, ordering, and metadata capacity. Test the
actual board commands against fake GPIO and retain host console smoke coverage.

### Dispatcher

Provide an allocation-free dispatcher in `daveos::core`, separate from scheduler
internals. The application passes the same `ModuleList` used by its scheduler and
its `SchedulerInterface<Event>&`. `dispatch(std::string_view)` returns `Status`.
Calls before normal dispatch starts or after shutdown return `not_running`; calls
from interrupt context return `invalid_argument`.
Applications assemble complete lines and dispatch them from normal scheduler
callbacks after successful initialization. Interrupt input must be buffered by
the application. All input shares one command stream; all output uses logging.

Modules expose constexpr command descriptor arrays containing command names,
short help strings, member-function callbacks, and C++ handler names. The
`DAVEOS_COMMAND(ModuleType, function, command, description, ...)` macro captures
the callback and C++ function name from one identifier. Handlers return `Status`
and receive either validated typed parameters or `CommandArguments` containing
only arguments after the module prefix and command. Raw argument views and typed
string views last until the handler returns.
Default task and command arrays are empty; command-only modules need no dummy
task. Modules without commands do not appear in routing or help.

Module names are static constexpr metadata exposed by `name()`, replacing the
base constructor's name argument. Distinct names for instances require distinct
template instantiations. Command prefixes default to module names and may be
overridden. Compile-time validation rejects empty or duplicate module names,
duplicate command prefixes, duplicate commands within a module, invalid callback
metadata, and reserved `help` collisions. Comparisons are ASCII case-insensitive.
Command names and prefixes contain only ASCII letters, digits, underscores, and
hyphens; display names may contain spaces if the command prefix is overridden.

The dispatcher owns fixed storage, with template defaults of 256 input bytes
and 16 arguments including prefix and command. Overflow rejects the entire line
without truncation or handler invocation. Tokenization happens exactly once.
ASCII whitespace separates arguments. Double quotes must surround whole
arguments; empty quoted arguments are preserved. Mixed quoted/unquoted forms,
unmatched quotes, and embedded NUL bytes are parse errors. Backslash escapes
only a double quote or another backslash, inside or outside quotes; other
backslashes remain literal. Input views need no terminating NUL. Blank lines
succeed without action. Nested dispatch on the same instance returns
`Status::busy` without changing active arguments; application code serializes
input rather than calling the dispatcher concurrently.

At both routing levels an exact case-insensitive match wins, otherwise a unique
prefix wins. Unknown names return `not_found`. Ambiguous matches, malformed input,
oversized lines, and argument overflow return `ambiguous_match`, `parse_error`,
`line_too_long`, and `too_many_arguments`, respectively. Argument
case is preserved. `help` prints the complete tree, while `<module>` and
`<module> help` print that module's commands. Extra arguments to these help forms
are errors. Help includes short command descriptions.

Help and errors use existing buffered logging under `core.command`. Handler logs
use the target module and its C++ handler function name; the previous logging
context is restored afterward. Normal filtering, truncation, and overflow apply,
including best-effort help output as for statistics tables. Logging failure does
not replace the command result. Without logging, handlers still execute and return
their statuses; help and diagnostics produce no output. There are no per-input
reply callbacks or sessions.

Add an interactive host console example with buffered stdin input and a
`console exit` command for orderly shutdown and log flushing. EOF has no command
meaning and must not stop the scheduler or cause a busy loop. The H563 and H755
examples share USART3 input with application-owned line buffering.
The shared H563/H755 STM32 UART console enables the peripheral FIFO in stage1
and queues up to 16 complete lines (up to 256 command bytes each), dispatching one per 1 ms
invocation. Queue overflow drops new whole lines and records a warning; this
is bounded burst buffering, not flow control for unlimited sustained input.

Both examples provide an application-owned USB CDC ACM console on CN13 (Type-C
on H563, Micro-AB on H755), using a shared pinned ST USB Device Library submodule. UART and USB retain separate
partial lines and echo while sharing command dispatch and log output. Each
transport is an independent scheduled module, registers a `CommandSource` with
the dispatcher through `bind_sources(CommandSourceList{...})` during initialization,
and registers its own logger subscriber. `uart_console` and `usb_console` Meson
options select either, both, or neither on both boards, independently of logging.
Sources submit complete lines from scheduler callbacks; all registered sources
share one tokenizer and dispatcher. Unregistered sources return `not_running`.
The dispatcher must outlive submissions; an empty source list is supported.
The board module only provides hardware commands. USB uses
fixed ping-pong output buffers with interrupt-driven transfers (PMA on H563,
FIFO on H755); disconnected
output is discarded and full buffers drop whole frames. DTR deassertion, USB
reset, and disconnect discard queued USB output and unfinished input. H563 has
no VBUS disconnect interrupt: suspend clears these buffers while retaining DTR
for normal resume. H563 attachment in both USB-C orientations, enumeration,
commands, physical reconnection, and reset recovery have passed hardware checks;
sustained backpressure and host suspend/resume stress remain to validate.

Test parsing, matching, boundaries, help, context restoration, nested calls, command-only modules, compile-time validation, and
allocation-free core operations on the fake platform, alongside existing host,
sanitizer, ARM compile, formatting, and lint checks.

Example log subscribers display elapsed time as `ddd:hh:mm:ss.mmm`, severity,
and a left-aligned `module.function` field so message text lines up. The default
context width is 22 characters, with display-only ellipsis for longer names.
Days use at least three digits; raw records retain microsecond timestamps and
complete context names.

Status diagnostics use enumerator names rather than numbers. A namespace-scope
X-macro builder generates an `enum class` with an explicit underlying type and a
`constexpr enum_name()` overload from one list. Names are borrowed static strings;
unknown values return `"unknown"`. Explicit values are supported without requiring
contiguous numbering; duplicate-value aliases are outside this helper's scope.

The UART console echoes buffered input in task context, clears the typed line on
Return, and logs the submitted command before dispatch. UART interrupts do not
transmit or log synchronously. Backspace/Delete support basic line editing, and
log output preserves unfinished input by erasing and redrawing it. Terminal-local
echo should be disabled; wrapped-line editing is outside the initial scope.

## Source organization

Colocate headers and implementations by component: shared console helpers live
under `console/`, core facilities live under
`core/{schedule,command,logging,queue,platform,enum}/`, and adapters under
`platform/{host,fake,stm32h5,stm32h7}/`, with shared adapter details in
`platform/detail/`. Optional networking lives in `net/` (`daveos::net`), with
shared STM32 Ethernet support in `platform/stm32/ethernet/`
(`daveos::net::stm32`). Include paths are relative to the repository root.
Core/platform namespaces remain `daveos::core` and `daveos::platform::*`. No separate include
and source trees are needed. Headers defining templates use `.hpp`, other
headers use `.h`, and C++ translation units use `.cpp`. Vendor and generated
file naming is retained. Meson definitions live with their components/targets.

Use fixed-width integers from `<cstdint>` for stored numeric values and explicit
enum underlying types. Use `PRI*` macros from `<inttypes.h>` for printf-style formatting of those values, rather than
casting to `long` or `long long`. Retain API-required types such as `int` for
`main`, printf width/precision, and C/POSIX return values, and `std::size_t` for
sizes and indices.

Do not rely on embedded libc to format 64-bit integers. Statistics retain their
full-width stored values but use a bounded 32-bit decimal display, appending
`+` above UINT32_MAX. Format any future 64-bit hexadecimal output as separate
32-bit upper and lower halves, with the lower half padded to eight digits.

Use small named concepts for repeated type contracts. `ModuleFor<M, Event>`
checks event-type compatibility at scheduling, cancellation, registration, and
command-dispatch boundaries after the concrete module is complete. Keep
value/metadata validation in constexpr checks. Prefer concrete parameter types
when no template deduction is needed, and name repeated policy predicates.

## Optional networking

Networking lives outside core in `daveos::net`, with pinned lwIP 2.2.1 in
NO_SYS mode. An application-owned service takes a driver and monotonic clock;
a thin CRTP module polls every 1 ms, processing at most four received frames
and servicing stack timeouts. PHY link polling occurs every 250 ms. Interrupts
never call lwIP. All packet and stack allocation uses fixed preallocated pools.
The first milestone supports Ethernet/ARP, IPv4, ICMP ping, UDP for DHCP, and
DHCP or application-configured static addressing. TCP provides a single-client
nonblocking byte-stream server for the console. IPv6, DNS, fragmentation, TLS,
and a general multi-listener/connection API are deferred. Future application
networking uses this separate library rather than extending SchedulerInterface.
Hardware-init failure leaves the remaining application operational and requires
reset to retry. Cable and DHCP recovery are automatic. `net status` reports link,
addressing, and counters. Meson `networking` defaults to false and disabled builds
need no networking submodules. Both STM32 board selections use ST HAL and LAN8742, with
fixed DMA buffers and board-specific RMII wiring. H755 hardware validation
passed for DHCP/static IPv4, ping, cable reconnection, and USB console
responsiveness. H563 initial hardware validation passed for UART/USB commands,
TX DMA, LEDs/button, timer completion, reset, DHCP, ping, and TCP commands.
USB works in both USB-C orientations; USB/Ethernet recover after physical
reconnection. Its
Cortex-M33 stack reservation is 64 KiB, enforced by MSPLIM. Long-lived STM32
application objects (modules, logger, scheduler, dispatcher, and transport
buffers) have file-scope storage rooted in `examples/stm32_console/appmain.cpp`.
Optional UART, USB, networking, and TCP components live in separate files selected
by Meson, with no application feature-selection preprocessor branches. A generated
`composition.hpp` assembles only selected members and their registration lists,
statistics routing, and shutdown calls; disabled components have no instances.
Component constructors are passive; hardware setup remains in module init.
Both boards build one `stm32-console` application target. `platform=stm32` and
`board=h563` (default) or `board=h755` select board files under
`platform/stm32/nucleo/`, the platform adapter, HAL, CPU/ABI flags, startup,
and linker script. Both use `meson/stm32.ini`; H755 additionally builds its
sleeping M4 image with separate CPU flags. Both
generated `Core/Src/main.c` entry points include `appmain.h` and call `appmain()`
after CubeMX peripheral setup; `appmain()` initializes the platform and calls
`application.run()`. Hardware setup waits for initialization;
Application binds command sources after both initialization stages succeed. The
64 KiB reservation addressed the old 34,216-byte application stack frame; the
current debug `appmain()` frame is 32 bytes. Total stack high-water usage has
not been measured, so the reservation is retained pending that measurement.
Keep linker and CubeMX settings consistent when resizing it. H755 has build
coverage only for the later static-storage, UART FIFO/16-line queue,
board/component-selection, and convenience-API changes; the current H755 firmware
still needs hardware testing.


The optional TCP console is an independent log subscriber and command source,
like UART and USB, and listens on port 1000 by default. A second connection is
reset while the first remains active. All commands use the existing dispatcher;
TCP callbacks only buffer bytes. Disconnect, peer FIN, or link loss discards
partial input and queued output; half-close is not supported. The next client
starts a fresh session. Fixed 4 KiB RX and 8 KiB TX buffers bound storage. RX uses
TCP flow control; output overflow drops a complete record without blocking.
Disconnected output is not retained. The protocol is plain TCP, without Telnet
negotiation, authentication, or encryption, and relies on local terminal echo.
`tcp_console=false` omits the console without disabling networking, UART, or USB.


Shared transport-agnostic console helpers belong in `console/`, namespace
`daveos::console`, with an explicit Meson dependency. `BufferedOutput` serves
asynchronous UART/USB transmission. A common CRTP console module handles task
scheduling, dropped-line reporting, command logging/dispatch, and independent
source/subscriber registration; transports retain session, echo, and I/O rules.
Prefer application ownership and borrowed callback contexts over singleton
objects. HAL/middleware APIs without user context may use a minimal callback
routing pointer, with teardown quiescing hardware before detaching it. lwIP's
global stack constraint does not justify a generic singleton guard.
TCP processes at most one completed line and 256 input bytes per invocation.
It consumes spans from a ring buffer without shifting remaining bytes. Bytes
after a newline remain for subsequent commands, partial lines persist between
invocations, and CRLF may cross chunk boundaries. No additional TCP command
queue is required.

## Application convenience APIs

`DAVEOS_TASK(Type, Function)` names a task using its C++ function identifier.
Existing explicitly named task descriptors remain supported. A module can call
`schedule<&Type::Function>(delay, mode)` and `cancel<&Type::Function>()`; the
scheduler interface additionally takes the target module. These typed forms
validate task registration at compile time. Runtime member-pointer forms remain
available for dynamic selection. Module lifecycle/event/sleep hooks have explicit
signature diagnostics at their CRTP boundaries.

Scheduling and timer delays accept integral `std::chrono::duration` values as well
as existing raw microseconds. Reject negative counts, non-integral microseconds,
and results at or above `kForever`, without changing pending work. Conversion is
exact and avoids intermediate overflow; floating-point duration representations
are not supported. Duration conversion errors are returned before lifecycle
checks. Existing rules remain: zero-delay one-shots are valid, while
zero repeat intervals and zero-delay interrupt timers are invalid. Existing
absolute-deadline saturation near the end of the clock range is unchanged.

Bound member timers can be requested/cancelled through
`timer<&Type::Function>(object, delay)` / `cancel_timer<&Type::Function>(object)`.
Module helpers supply `*this` automatically. These callbacks retain interrupt
context and existing replacement, capacity, cancellation, and lifecycle semantics.
Identity relies on distinct address-taken functions, including the invocation
specializations for bound members. Unsafe linker folding such as `--icf=all`
is unsupported; the supplied builds do not enable it. Identical callback bodies
must still have independent identities under supported compiler/linker options.

Optional `core::CommandBinding<Event, Sources>` copies source pointers during
passive construction. `make_command_binding<Event>(CommandSourceList{...})`
deduces their count. Register it as a module and call `connect(dispatcher)` before
init/run; it binds sources in stage2. A missing connection returns `not_running`
and fails initialization. Its module name is `commands`; applications using it
must reserve that name. Direct application wiring remains supported.

The standalone `starters/application` project supplies one application module,
a real-time host binary, and a fake-time test, consuming DaveOS through Meson
dependencies. Export `daveos-core`, `daveos-console`, the selected adapter
(`daveos-host`/`daveos-fake`/`daveos-stm32h5`/`daveos-stm32h7`), and optional
`daveos-network`. The host configuration also exports the fake adapter. Consumers
can disable framework examples/tests and pin the wrap revision independently.

## Application composition and periodic tasks

The convenience layer keeps explicit scheduler/logger/dispatcher APIs available
for custom composition. Prefer chrono durations in new examples/conveniences.

### Application composition

The factory accepts four combinations:

```cpp
auto app = core::make_application<Event>(platform, modules);
auto app = core::make_application<Event>(platform, modules, logger);
auto app = core::make_application<Event>(platform, modules, sources);
auto app = core::make_application<Event>(platform, modules, logger, sources);
```

Here `modules` is a ModuleList, `sources` is a CommandSourceList, and `logger`
is an application-owned Logger. Each line is an alternative. Application owns
its scheduler and, only when commands are selected, its dispatcher. It borrows
the platform, modules, logger, and sources. Capacities remain compile-time
settings; defaults match the existing components. No heap, virtual dispatch,
hidden registered module, reserved module name, or explicit connect() call.

Construction stores references and static metadata without touching module
instances or hardware. All borrowed objects must finish construction before
init/run and outlive the Application. Application is non-copyable/non-movable
because its members refer to each other; the factory returns a prvalue using
C++17 guaranteed copy elision. File-scope instances remain supported. Include `core/schedule/application.hpp`.
Factory numeric arguments are event/timer capacities (32/16 by default), followed
by command line/argument capacities (256/16) on command-enabled overloads.

app.init() runs scheduler initialization, then binds command
sources only after successful completion of both stages. app.run() calls init()
if needed before dispatch. Repeated init/run and failure behavior follow the
existing scheduler rules, with initialization_failure() forwarded. Expose
scheduler() for advanced use, but document that lifecycle entry is through
Application. Logging and commands remain independently optional, including
commands with no logger. No third module initialization stage is introduced.

The helper binds sources after all stage2 hooks, before dispatch, rather than
during a registered module's stage2. Commands may not be submitted during initialization through the helper. Explicit low-level
CommandBinding remains available for applications requiring its stage2 wiring.

### Declarative periodic tasks

TaskDescriptor has an optional period (zero means no automatic scheduling).
Declare a periodic task with:

```cpp
static constexpr auto tasks() {
  return std::array{DAVEOS_PERIODIC(Worker, Poll, 10ms)};
}
```

The helper accepts an integral chrono duration, validates exact positive
microseconds at compile time, and registers the callback/name once. The first
iteration is due one period after normal dispatch starts, then follows the
existing repeat cadence and overdue-iteration rules. Immediate-first execution
and dynamic periods continue to use explicit schedule().

Install these default schedules once after registration validation and module
binding, before any stage1 hook. Thus any explicit schedule/cancel during either
stage overrides the default, including cancellation from another module's init.
Successful explicit scheduling/cancellation before init also overrides defaults.
Initialization failure discards these schedules with other pending work.
Construction does not start clocks, schedule hardware, or call module methods.

Tests cover all four composition combinations, passive
construction, two-stage ordering, failure/repeated lifecycle calls, source
binding, file-scope use, and allocation-free operation. Periodic tests cover cadence/start epoch, explicit overrides/cancellation, init failure,
compile-time invalid durations, and unchanged explicit task descriptors.


## Ready-made I/O, console library, and device starter

`platform/host/io.hpp` provides `host::stdout_subscriber()` with the standard
prefix, implicit newline, and per-record flush. `host::run(runnable)` accepts a
Scheduler or Application, returns 0 for Status::ok and 1 otherwise, and reports
failure status to stderr independently of DaveOS logging. Both helpers can be
used with a fake platform and introduce no timer thread themselves.

Reusable USART3/USB transports, Board commands, Nucleo network wiring, and the
Console group live in `platform/stm32/console`, namespace
`daveos::platform::stm32`. The transport-agnostic serial module wrapper and TCP
console live in `console/`, namespace `daveos::console`. Library code depends on
core/console/net and a consumer-supplied board contract, never on example headers.
`TransportModule<Event, Transport>` owns a passive transport, initializes it in
stage1, and supplies the common command/log/statistics interface. UART and USB
expose the same shape; UART retains its FIFO and 16-line queue. A second active
USART3/USB instance is rejected; C callback routing pointers own no state.

`stm32::Console{uart, usb, tcp}` borrows any number of transports. It provides
modules(other_modules...), subscribers(), sources(), log_statistics(), and
reverse-order stop(). It creates no scheduler/module of its own and reserves no
names. Network services remain separately owned and outlive their TCP consoles.
The STM32 example's generated header selects objects; registration and console
shutdown use this library group.

Configured Nucleo startup, linker scripts, and peripheral glue live in
`platform/stm32/nucleo/{h563,h755}`. `stm32_support=true` enables their dependencies
without building the example. Export `daveos-nucleo`, `daveos-stm32-console`,
`daveos-stm32-uart`, and conditional `daveos-stm32-usb` Meson dependencies.
The board dependency propagates CPU/ABI and linker settings and compiles sources
in the consuming application. The consumer provides appmain.h/appmain(). H755
also provides a sleeping M4 image; programming must include both images.

`starters/stm32` is a separate Meson consumer with its own appmain(), periodic
Worker, and UART board console. Board selection defaults to H563. A mismatch
between the starter's board and the subproject's board is an error. Startup uses
our CubeMX configuration, not a board BSP. The host/fake and device starter wrap
pins must advance when new public APIs they consume are committed.

The documentation learning path has four levels: a small hello, logging and
commands, hardware console, and custom components. README links to these levels;
docs/reference.md retains detailed build, programming, and API information.
Validate actual host stdout/stderr/exit codes, common console registration and
reverse shutdown, existing USB/TCP behavior, and separate H563/H755 starter builds
and programming plans. Hardware claims remain distinct from build coverage.

The reusable full console has passed H563 UART/USB/TCP, Ethernet ping, timer,
and software-reset recovery checks. The standalone H563 starter has passed
periodic-worker, UART help/timer/statistics, and reset-recovery checks. H755
starter and console validation for this extraction is build/programming-plan
coverage only; its current firmware still needs hardware validation.
