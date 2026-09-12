# DaveOS

## Purpose

Build a simple, non-preemptive task scheduler suitable for embedded projects.

The first embedded application will flash LEDs on a Nucleo board using an
STM32H563. This provides a small application for exercising the scheduler and
platform support after the initial host implementation.
Longer-term applications may include multiple UARTs, CAN buses, and an Ethernet driver.

## Language and Tools

* Implement in C++20.
* No exceptions.
* No dynamic memory allocation in the core or embedded code.
* The basic host platform should avoid allocation where practical, but may allocate
  when necessary. Test infrastructure may allocate freely.
* Standard library facilities are allowed subject to these allocation rules.
* Use `constexpr` where possible.

## Design

DaveOS has a hardware-independent core with platform support injected at
construction. The core depends only on permitted C++ standard library facilities.
Platform implementations may depend on an OS or other libraries.

The core consists of a scheduler and one or more modules.

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

A module is a class with one or more schedulable member functions.

A module also has:

* A name.
* A `can_sleep()` callback that indicates whether the module permits system sleep.
* An `on_event()` callback that receives an application-defined event enum value.
* An `init(InitStage stage)` callback used at startup.
* A reference or pointer to the module-facing scheduler interface.

Each module exposes a compile-time array of task descriptors through an accessor.
Each descriptor pairs a human-readable string name with its schedulable member
function. The array defines the task callbacks and their names; its size provides
the task count without a separate count accessor. Statistics tables identify tasks
by module name and task name.
Module names must be unique within a scheduler. Task names must be unique within
their module; different modules may use the same task names.

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
The application constructs and owns the module instances, passing their pointers
in a typed `ModuleList`, for example `ModuleList{&fooMod, &barMod, &bazMod}`.
The list preserves each module's concrete type so module counts and task descriptor
array sizes can be derived at compile time. It holds pointers rather than owning
the modules; module instances must remain alive for the scheduler's lifetime.

The scheduler implements `SchedulerInterface<Event>`, a small module-facing
interface parameterized only by the application's event enum. It exposes
scheduling and cancellation, event posting,
timers, and logging without exposing the concrete scheduler's module-list types or
storage capacities. Modules depend on this interface rather than the full scheduler
template specialization, so they can be reused across scheduler configurations.

During construction, the scheduler automatically binds a reference or pointer to
this interface into each registered module. This binding is available after
scheduler construction and before any `stage1` callback. Module constructors run
before binding and must not access the scheduler through that reference.

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
average execution duration. Include diagnostic counters such as event and timer
overflows, dropped logs, and truncated messages.

### Error reporting

Define a common `enum class` for errors the scheduler can report or encounter.
Use explicit status returns rather than exceptions, with a success value for
operations that complete normally. Distinguish error conditions such as repeated
initialization, module initialization failure, invalid repeat interval, event
queue overflow, timer overflow, and `not_running`. The complete enumerator set
and type name remain to be finalized.
`not_running` consistently reports that an operation requires an active scheduler
run. It is an explicit error for both a pre-run `stop()` call and a timer request
during initialization, rather than a silently accepted operation.
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

DaveOS timer requests require a strictly positive delay. A zero-delay request
returns an error without creating a timer or modifying an existing timer.

Timer requests made before the scheduler starts normal dispatch return
`not_running` and otherwise do nothing, including requests during initialization.
They do not reserve a slot, arm the platform timer, or carry forward into execution.
Applications needing timers
during initialization must arrange them outside the DaveOS timer API.
This intentionally differs from task scheduling: tasks may be registered for later
dispatch during startup, while DaveOS interrupt-time timers are a running-scheduler
facility and do not provide initialization-time timing services.

Requesting a timer for a callback that already has a pending timer replaces that
timer's due time rather than creating another timer. Applications needing separate
timers for the same underlying behavior use distinct wrapper functions.

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

Logging is accessible to all modules.

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
- subscribers are supplied at construction time, with fixed subscriber storage,
  and are available for initialization diagnostics
- logging is buffered and may be called from module callbacks and interrupt
  handlers; logging calls enqueue records without invoking subscribers
- subscribers receive buffered records later in the scheduler's execution
  context, outside interrupt handlers
- Each subscriber-facing record includes timestamp, severity, message text,
  module name, and task name. Attribution must be captured when the log call
  occurs and retained for deferred delivery. Calls outside a module task use
  descriptive context labels such as `core/init` or `interrupt`. Interrupt logs
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

The first implementation pass targets the host, including fake-time unit tests
and real-time integration tests. ARM support through the STM32H5 HAL follows in
a later pass; the core remains hardware-independent from the outset.

* Host: clang++.
* ARM: latest arm-gcc and STM32H5 HAL; pin concrete versions when setting up the build.
  The initial MCU target is STM32H563.

Host support will provide both:

* A fake clock and timer for fast, deterministic unit tests without real-time waits.
  Tests can advance time manually or enable automatic advancement to the next due
  task or timer when the scheduler would otherwise wait.
  Advancing time delivers due timer callbacks synchronously before the advance
  operation returns. Callbacks run in simulated interrupt context and follow the
  same interrupt serialization and critical-section rules. Fake time requires no
  dedicated timer thread.
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

Use Meson. Add Python build helpers as needed, with dependencies managed by uv.

## Style

* Emphasize simplicity and concision in design and implementation.
* Be DRY (Don't Repeat Yourself): keep each piece of logic or knowledge in one place.
* Use the Google C++ style guide.
* Allow short variable names if their physical scope, from first to last textual
  appearance, is less than 15 lines.
* Do not repeat the class name in member names.

## Open decisions from specification review

* Platform setup: confirm the STM32H563 Nucleo board configuration and select the
  clock/timer source when implementing the embedded platform.

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

Exact C++ signatures, status enumerator names, timer rearming mechanics, and storage
layout can be settled during implementation while preserving this specification.
