# Cooperative task yielding

A task may call `scheduler().yield()` directly or through a helper, including
inside a synchronous library's disk adapter. It executes at most one other
due task, chosen earliest-scheduled-first among eligible tasks, and returns.
No work means an immediate return. Interrupts remain enabled during callbacks.
Events and logging delivery stay with the outer dispatch loop; yield never
sleeps or advances the fake clock.

This uses the existing C++ call stack. If A yields to B and B yields to C,
C must return before B resumes, and B must return before A resumes. It is not
a coroutine or a preemptive thread. Every callback already on the stack is
excluded from selection, even after self-rescheduling. Overdue repeating
iterations remain pending; generation checks preserve explicit schedule/cancel
changes made during nested callbacks.

## Context and results

Only a scheduled task's call chain may yield. Initialization, event callbacks,
command handlers (even when reached from a console polling task), log
subscribers, idle callbacks, interrupts and unrelated host threads are rejected.
A helper called by an ordinary task retains permission. A command handler's
helpers do not. Context guards restore the previous callback kind, scheduler
identity and logging names on return. Platforms must provide thread-local
context on multithreaded hosts and reliable interrupt detection.

| Result | Meaning |
| --- | --- |
| `ok` | One other task ran and returned. It may have requested stop. |
| `empty` | No eligible due task. No time advancement or waiting occurred. |
| `invalid_context` | Invalid caller; counted in `invalid_yields`. |
| `depth_limit` | Eligible work exists, but nesting would exceed the limit; counted in `yield_depth_errors`. |
| `not_running` | Stop requested; no further nested work dispatched. |

`Capacities{.yield_depth = 4}` is the default. The count includes the outer
callback, so 4 permits at most three nested callbacks. Values 0 or 1 disable
nested dispatch. The low-level factory accepts the limit after modules (or
after the logger when supplied). This bounds callback nesting, not stack
bytes; each application must still budget stack for its callback call chains.

## Resource ownership and waiting

Never wait in a nested callback for a resource held by a suspended ancestor:
the ancestor cannot resume to release it. For example, a filesystem service
may hold exclusive ownership while its disk adapter yields; another task's
filesystem request must return `busy` or queue work for later. There is no
automatic lock-cycle detection. The same rule applies to dependency cycles
between tasks without explicit locks.

A wait loop must handle yield results and the operation's own timeout. Even
when stop is requested, the caller must keep borrowed buffers alive until its
asynchronous operation completes or has safely stopped accessing them. The
scheduler waits for all nested callbacks to return before quiescing timers
and discarding pending work. Yield does not drain events/logs, so long waits
can accumulate those queues.

With a fake platform, a test must explicitly advance time or inject completion
while such a loop runs; repeated empty yields do not advance to a timer. A
real-time platform's interrupt-driven transfer/timer can complete independently.

## Statistics

Existing min/max/average durations remain elapsed callback time, including
nested execution. `total_nested_duration` counts nested callback time once,
and `total_self_duration` excludes it. Self time still includes interrupts,
waiting and dispatch overhead; it is not a CPU profiler. Completed-iteration
counters advance only when the callback returns, so watchdog progress policies
must allow the duration of the complete operation, including yielded time.

The statistics table includes self/nested totals and context/depth errors.
Snapshot/reset covers these counters with the existing statistics API.

`tests/yield/` covers selection, reentry prevention, backlog, nested accounting,
context restoration/rejection, depth limits and stop unwinding. The optional
[FatFs worker](storage.md) uses this primitive while waiting for SD transfers.

A pending wait repeatedly yielding with no eligible work still spins: yield
does not invoke idle callbacks, enter WFI, or sleep. Bounded protocol polling
can shorten this period, but battery-powered applications need a separate
idle/wait design rather than assuming yield reduces power consumption.

## Waiting for buffer release

`core/schedule/wait.hpp` provides `core::wait_for_release(ready, pump, now, timeout)`.
The callables are borrowed and allocation-free; `now()` returns monotonic
microseconds and the timeout is an exactly representable chrono duration.
For example, after an asynchronous operation has accepted borrowed buffers:

```cpp
auto waited = core::wait_for_release(
    [&] { return completion.ready(); },
    [&] { return scheduler().yield(); },
    [&] { return platform.now(); }, 250ms);
// Buffers are now released, even if waited reports timeout or not_running.
// Inspect completion.result() separately for the actual operation outcome.
```

`ready()` must mean ownership has returned, not merely that a deadline passed.
The first observed timeout or pump error is retained. `ok`, `empty`, and
`depth_limit` are normal pump results. A stop request or invalid context does
not permit an early return with live buffers. Invalid timeout conversion also
records `invalid_argument` and drains to completion. Readiness is checked before
the deadline on each iteration, so already-completed work wins; this is an
observed wait deadline, not a measurement of the interrupt completion instant.

The helper does not start, cancel, retry or reset anything. Its deadline is
**not an upper bound on return time**: the HAL must independently bound its
transfer and cleanup. A broken backend that never releases buffers can block
forever. Completion must not depend on a task which cannot run after scheduler
stop or at the nesting limit. Fake tests can advance time in `pump`; real
interrupt-driven backends continue independently. No sleeping is added.

The watchdog integration test runs a real nested callback during such a wait:
the suspended repeating task has zero completed iterations until it returns.
An insufficient progress allowance reports `health_failed`; an allowance that
covers the whole operation passes. Configure that allowance deliberately;
yielding alone is not evidence that the waiting task has made progress.
