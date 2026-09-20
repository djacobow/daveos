# Platforms and injected hardware contracts

## Specification map

These four documents together define the agreed behavior. Tutorials show usage;
these specifications own the contracts. Change the owning spec when behavior
changes, and link to it rather than duplicating requirements.

| Specification | Scope |
| --- | --- |
| [Concept and core](../../PROJECT.md) | Project principles, modules, scheduling, events, timers, queues, logging, commands, composition, and state machines. |
| [Utilities and services](services.md) | Reusable networking, console plumbing, CRC/version utilities, boot/update policy, watchdog health checks, and OTP storage/modules. |
| [Platforms](platforms.md) | Injected platform contract, host/fake adapters, file-backed hardware models, and build/programming conventions. |
| [STM32 integration](stm32.md) | H563/H755 board support, console integration, H563 boot/OTA/OTP hardware, and qualification boundaries. |

This document defines platform contracts and host/fake implementations.
Board-specific composition and hardware qualification live in the
[STM32 specification](stm32.md).

## Contents

- [Platform interface](#platform-interface)
- [Supported platforms](#supported-platforms)
- [Build system](#build-system)
- [Host I/O helpers](#host-io-helpers)
- [Injected flash and host file backend](#injected-flash-and-host-file-backend)
- [OTP backend contract](#otp-backend-contract)

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

All STM32 builds, including dependencies, M4, and starters, use `-Os` with debug
information retained.

Use Meson. Use uv for Python test dependencies: pytest for portable checks and
pinned Watcher/pyserial for optional HIL. Virtual environments and uv caches
live under build/. Production packaging/programming tools remain standard-library Python.
Build configurations live under `build/` (for example `build/host`, `build/fake`,
`build/asan`, `build/arm`, and `build/h755`). All generated intermediates and caches belong under
that ignored directory and can be removed and regenerated.
Provide `format`, `format-check`, and `lint` build targets using clang-format and
cppcheck.
STM32 examples provide explicit `flash` (CubeProgrammer), `flash-openocd`, and
`flash-plan` targets. Programming builds and verifies the firmware before resetting;
H755 includes both core images. Tool paths and ST-LINK serial are configurable.

## Host I/O helpers

`platform/host/io.hpp` provides `host::stdout_subscriber()` with the standard
prefix, implicit newline, and per-record flush. `host::run(runnable)` accepts a
Scheduler or Application, returns 0 for Status::ok and 1 otherwise, and reports
failure status to stderr independently of DaveOS logging. Both helpers can be
used with a fake platform and introduce no timer thread themselves.

## Injected flash and host file backend

Boot policy and the OTA engine receive an injected `boot::Flash` interface.
Host/fake simulations can use `platform::host::FileFlash`, a raw persistent file
with caller-supplied geometry and an optional injected clock. It models bounds,
sector erase, aligned 16-byte programming, and one-to-zero NOR bits, with fsync
before successful completion. Reopening preserves images and metadata across
simulated boots. It does not model STM32 ECC or torn in-flight mutations; the
memory-backed fault-injection tests cover those failure boundaries separately.

See [boot/update policy](services.md#bootloader-ota-and-reliability-services)
for the portable behavior implemented against this interface.

## OTP backend contract

The host file backend must preserve contents across close/reopen and support
tests of slot consumption, repeated types, invalid records, partial writes and
exhaustion. Persist permanent block locks as well as contents across reopen.
It should model the selected OTP programming restrictions; ordinary
flash erase/rewrite semantics are not sufficient.

The [OTP service](services.md#otp-storage-agreed-behavior-and-implementation-design)
owns record/cache semantics; [STM32 adapters](stm32.md#otp-integration) own
hardware access and qualification.

### FileOtp

Provide platform::host::FileOtp with a passive constructor taking a borrowed
path and explicit existing/create mode. Open during init, matching the stage1
contract. Create is exclusive and never truncates an existing file; existing
mode rejects missing, malformed, wrong-version or wrong-size files. No automatic
reset or repair of a damaged backing file. Use bounded buffers and positional
file I/O; successful mutations include persistence synchronization.

Define a versioned file format: a 64-byte header describing the magic, format,
geometry and header CRC, followed by 32 fixed 128-byte block images. Each image
contains the 64 logical bytes, one state byte for each of its 32 halfwords,
a persistent lock byte and reserved padding. Per-halfword states distinguish
virgin, programming attempted/incomplete, programmed, and injected read failure.
A programmed 0xFFFF halfword is not virgin. Lock state is sticky across reopen.
Reject malformed simulator bookkeeping instead of treating it as erased.

Before each simulated halfword write, persist its attempted state, then its data,
then its completed state. Program length first, then the remaining non-type
halfwords, and the type halfword last. Program each halfword at most once,
including 0xFFFF padding. Data and lock faults are independently injectable at
operation boundaries. Model failure before any mutation, partial programming,
readback mismatch/ECC, and lock failure before/after persistence. Close/reopen
must retain evidence of interrupted writes and lock state. Reset fixtures by
creating a new test file, not by exposing erase through the service.

This is a deterministic OTP model, not a claim about transistor-level failure
behavior or storage guarantees after loss of power to the host itself.
