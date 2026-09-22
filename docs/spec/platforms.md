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
| [STM32 integration](stm32.md) | H563/H755 board support, console integration, H563/H755 boot/OTA, H563 OTP hardware, and qualification boundaries. |

This document defines platform contracts and host/fake implementations.
Board-specific composition and hardware qualification live in the
[STM32 specification](stm32.md).

## Contents

- [Platform interface](#platform-interface)
- [Supported platforms](#supported-platforms)
- [Build system](#build-system)
- [Host I/O helpers](#host-io-helpers)
- [SPI and I2C HAL](#spi-and-i2c-hal)
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

## SPI and I2C HAL

The portable HAL and initial IRQ backends implement the contract below. See
[the API and wiring guide](../spi-i2c.md) for concrete types, examples, and
validation scope.

### Namespaces

Use `daveos::hal::Status` for the shared SPI/I2C status enum and
`daveos::hal::spi` / `daveos::hal::i2c` for portable bus interfaces and actions.
Concrete backends belong under `daveos::platform::stm32h5`,
`daveos::platform::stm32h7`, and `daveos::platform::fake` as appropriate. Keep
the portable HAL separate from scheduler internals; its timer dependency is
injected through an adapter.

### Agreed direction

- Provide a stable DaveOS interface between applications/device drivers and
  vendor HALs. Inject concrete platform backends; keep vendor handles and GPIO
  details in platform/application wiring rather than portable device drivers.
- Both SPI and I2C initially support controller/master operation only: DaveOS
  initiates transactions. SPI slave and I2C target operation are outside the
  initial scope.
- Support nonblocking transfer initiation with callback-only completion and a
  reusable completion helper for callers that prefer polling. Do not add
  blocking transfers or a separate polling completion mechanism in the HAL.
- Support multiple configured hardware instances, such as SPI1 and SPI2, with
  build-time aliases, potentially application-defined enums.
- Configuration must accommodate the backend's required peripheral specifiers,
  handles, and GPIO assignments. Follow the existing passive-constructor and
  explicit-initialization rules.
- Maintain independent state and synchronization for independent hardware
  instances. Requests using the same physical hardware must not collide.
- If the requested controller is occupied, `start()` immediately returns
  `busy`, without disturbing its current transfer or retaining the rejected
  request. The HAL does not queue requests or retry them. Callers may layer a
  queue and drain task above this API to provide queued submission.
- SPI must account for multiple attached devices sharing the bus pins but using
  different chip-select pins.
- Maintain read/write attempt and error statistics per attached device, with
  controller-wide totals and separate submission-rejection counts.
- Follow the project's allocation-free, injected-dependency, CRTP/static-dispatch
  and structured-state-machine conventions.

### Completion contract

`start()` returns success when a request is accepted, or an immediate error
such as `busy` when rejected. Every accepted request invokes its completion
callback exactly once, carrying success/error information. Rejected requests
never invoke their callbacks.

An accepted transfer's completion interrupt may run before `start()` returns.
Callers must prepare callback state before submission; the polling helper must
also establish its pending state before calling `start()`. Neither may reset
completion state after acceptance in a way that loses an early completion.
An early completion does not change the submission result: `start()` reports
acceptance, while the callback reports the transfer outcome.

Completion callbacks run in interrupt context and must remain short. Provide
an allocation-free helper whose callback saves the result and publishes a
completion flag for task-context polling. Publication must safely synchronize
the result and flag across interrupt/task contexts, including concurrent host
simulation; a plain unsynchronized flag is insufficient. The helper does not
add another HAL completion path.

Before invoking a completion callback, finish hardware cleanup, release chip
select where applicable, and release the controller's transfer ownership.
Invoke the callback without holding an internal lock. The callback may call
`start()` to begin another transfer, including on the same controller. This
submission follows the normal acceptance/`busy` rules; completion does not
reserve priority for the callback's next request.

The preceding transfer's completion path must not subsequently clear or alter
the newly started transfer's state. This permits interrupt-context transfer
chaining without a scheduler round trip.

Completion callbacks receive the original transaction references alongside the
result, so they can inspect received data and reuse the action list and buffers
in a new invocation. These refer to caller-owned storage, not HAL-owned copies.

The completion result contains an overall status (success, timeout, or a
specific transfer error) and the number of fully completed actions. The count
identifies the completed prefix of the action list; it includes completed
pauses. No partial-byte count or guarantee about progress within a failed action
is provided in the initial API.

Use one shared portable HAL status enum for SPI and I2C, covering submission
errors (such as busy, invalid arguments, and unsupported buffers) and transfer
outcomes (such as success, timeout, I2C NACK, arbitration loss, and hardware
failure). Name this enum `daveos::hal::Status`. Backends map vendor errors into
this enum. Keep vendor-specific
diagnostic details accessible through the backend, outside the portable
completion result. The enum contains `ok`, `busy`, `invalid_argument`,
`unsupported_buffer`, `not_initialized`, `initialization_failed`, `timeout`,
`timer_error`, `nack`, `arbitration_lost`, `hardware_error`, and `faulted`.

Interrupt callbacks may submit transfers, request reset under the contract
below, and publish completion through the polling helper. Statistics snapshots
and clearing are task-context-only; callbacks that need statistics processing
must defer it to task context.

### Polling completion helper

Provide a reusable helper supporting one outstanding transaction. It prepares
its completion state before calling `start()`, preserving completion even if
the callback runs before submission returns. `ready()` indicates completion;
`result()` exposes status, completed-action count, and the original transaction
references. Retain the result until the helper is reused.

Attempting to reuse a helper with a pending transaction returns `busy`, leaving
its pending operation and completion state untouched. The helper owns no action
lists or payload buffers; their lifetimes remain the caller's responsibility.
Safely publish completion and the result across task/interrupt contexts.
If the HAL immediately rejects a submission, the helper publishes a ready
result containing that error, zero completed actions, and the original
transaction references; it does not synthesize a HAL callback. An attempted
reuse while already pending still returns `busy` without changing the existing
operation or publishing a replacement result. `result()` returns an optional
value, empty before readiness; once ready, it contains the published result.

### Controller/device registration and ownership

Use separate application-defined enum aliases for controllers and attached
devices, resolved through fixed configuration tables. For example:

```cpp
enum class SpiBus { external, sensors };
enum class SpiDevice { sd_card, accelerometer };
```

Controller entries supply the backend handles, peripheral specifiers, and bus
pin assignments required by the platform. SPI device entries reference a
controller and specify CS pin/polarity, speed, mode, and bit order. Apply the
device's configuration when acquiring its controller. I2C device entries
reference a controller and specify their 7-bit bus address. Configure one I2C
speed per controller, shared by every attached device; the application chooses
a speed suitable for all devices on that bus. Registration is fixed at build
time, not dynamic.

SPI transactions use the attached device's configured speed. Do not provide a
per-transaction speed override in the initial API; revisit speed changes only
if a concrete device integration requires them.

Fail initialization if any configured controller or attached-device entry is
invalid, and identify the failing entry in the error diagnostics. Validate
aliases and controller references at compile time where possible; validate
backend handles, pins, and other runtime-only properties during initialization.
Do not silently omit an invalid entry and report successful initialization.

Validate all configuration before beginning hardware setup. If hardware setup
fails partway through, disable controllers already initialized by this registry
and safely quiesce any partially configured controller. Leave configured SPI
chip selects inactive and keep the entire registry unavailable; no device
handle may submit a transfer through a partially initialized registry. Report
the failing configuration entry and error. Initialization is all-or-nothing,
not partial availability of the controllers that happened to succeed.

Reject registration of the same physical controller under multiple bus aliases,
two SPI device entries using the same physical CS pin, or two I2C device entries
using the same address on one controller. The same I2C address on different
controllers is valid. These checks preserve a single ownership/synchronization
domain for each physical controller and unambiguous device selection. Check at
compile time where configuration permits, otherwise during initialization.

The STM32H5 SPI backend manages CS through GPIO outputs rather than the SPI
peripheral's hardware NSS output. See the
[STM32 fixture and integration requirements](stm32.md#spii2c-validation-fixtures).

The portable HAL does not prescribe how a requested clock rate maps to a
hardware-supported rate: no universal exact-rate, maximum-rate, or rounding
rule is imposed. A platform implementation may offer such a policy as a
constructor/configuration option. Constructors remain passive; applying the
configuration follows the existing initialization/transfer rules.

For the STM32 backends, treat the requested speed as a maximum: select the
closest supported rate that does not exceed it, expose the effective configured
rate, and use that rate for automatic timeout estimation. Fail initialization
if no suitable rate is available. This is a platform configuration policy,
not a mandatory rule for every implementation of the portable HAL.

The portable transaction API does not select interrupt-driven versus DMA
transfers. The backend's controller configuration selects the mechanism;
application drivers use the same action descriptors, borrowed-buffer contract,
and completion callbacks either way. Supported mechanisms and any associated
buffer constraints must be defined by each backend.

If the configured backend cannot directly use a supplied buffer, reject
`start()` before bus activity and without invoking a completion callback.
Backend buffer restrictions are part of whole-transaction validation. Do not
copy through internal staging buffers or silently switch transfer mechanisms
to accommodate unsupported buffers.

The platform backend performs any required cache maintenance before transfer
and before delivering completion, so application device drivers do not manage
platform cache details. Backends may impose documented buffer alignment and
memory-region requirements; validate these before accepting a transaction.
Completion must make received data available to the caller under the borrowed
buffer contract, including after failure or timeout cleanup.

Distinguish a **bus/controller** (SPI1, SPI2, I2C1) from an **attached device**
(an SPI target with its own CS, or an I2C target with its own address).
An attached-device handle references its controller and configuration. Devices
on one controller share its transfer ownership and synchronization; different
controllers can operate independently.

Use short synchronization sections to protect shared state, and explicit
in-flight ownership to reserve the controller across an asynchronous transfer.
Do not keep an ordinary mutex locked until an interrupt completes the transfer.

`start()` is callable from task and interrupt context. Acquiring controller
protection must be nonblocking: if protection cannot be acquired immediately,
return `busy` without retaining the request or invoking its callback. This
applies both to independent submissions and to submissions from completion
callbacks. Synchronization must be safe for the calling context; no blocking
mutex acquisition or spin-wait for another owner is permitted.

### Application-facing device handles

The registry provides a small, non-owning device handle. Application wiring
selects the registered device by its application-defined alias and injects the
handle into the portable device driver. The driver need not know the alias enum,
registry template parameters, or concrete platform type.

The handle is allocation-free and copyable by value, holding borrowed device
context and a function-pointer operation interface. Backend implementation
continues to use CRTP/static dispatch. The registry resolves controller and
device configuration and enforces shared-controller ownership for operations
submitted through any handle copy.

Creating a handle is passive: no hardware access and no requirement that HAL
initialization has run. Handle construction/binding must follow the project's
construction-order independence rules. Transfer/reset operations before the
registry is ready return `not_initialized`. The registry must outlive all
handles and outstanding operations. Handles own neither controllers nor buffers.
The application owns registry and drivers; the registry owns bus state; drivers
own their transaction storage.

Application device-alias lookup:

```cpp
auto sd_device = spi.device<SpiDevice::sd_card>();
SdCard card{sd_device};
```

The portable driver stores a `daveos::hal::spi::Device` handle by value. Use the
same injection pattern for I2C devices. Device handles expose transaction
submission and per-device statistics. Controller reset and controller-wide
statistics are registry operations selected by bus alias, for example
`buses.controller<SpiBus::external>().reset(on_reset)`. They remain application-level operations
because they affect hardware shared by all attached devices on that controller.
Device lookup is compile-time through a template alias, as shown above. An
alias absent from the registered device table is a compile-time error. Runtime
device lookup is outside the initial API; apply the same rule to SPI and I2C.

### SPI transactions and pauses

The initial SPI API uses 8-bit words and byte buffers only. Wider application
values are encoded into bytes by the caller; configurable hardware word sizes
are outside the initial scope.

A single `start()` accepts an ordered list of actions forming one transaction.
For ordinary transactions, acquire the controller, apply the attached device's
configuration, assert its
chip select, and execute the actions in order. Keep controller ownership and
CS asserted across action boundaries. After the last action, deassert CS,
release ownership, and deliver one completion callback. An error stops the
remaining actions, performs cleanup, and delivers failure.

Actions include write, read, simultaneous exchange, and pause:

```cpp
std::array actions{
    spi::write(command),
    spi::pause(10us),
    spi::read(response),
};
```

An SPI read transmits a fill byte while receiving. The default is `0xFF`,
overridable per read action, for example `spi::read(response, 0x00)`. A read
does not require the caller to provide a separate fill buffer.

An SPI exchange requires equal-length, non-overlapping TX and RX buffers.
In-place exchange is outside the initial scope. `start()` rejects an exchange
that violates these requirements before accepting or executing the transaction;
as with other rejected requests, no completion callback is invoked.

`pause()` delays the next action by at least the requested duration without
blocking. Keep CS asserted and the controller reserved throughout the pause;
other controllers and scheduler tasks may continue. An injected timer resumes
the transaction. Timer capacity and failure handling follow the injected
timing contract below.
General CS changes are not actions in the initial API; devices that need CS
released between command and result use separate transactions. A dedicated
unselected-clock operation is the exception described below.

### SPI response checks

`spi::check_response(bytes, expected, mask=0xff, idle=0xff)` examines 1–32
borrowed bytes already populated by an earlier read, without clocking or
releasing CS. Skip leading idle bytes, then compare the first non-idle byte
under the mask. No response or mismatch completes with `response_mismatch`
and prevents all later actions. The scan is bounded and runs in interrupt
context, invokes no application code, and adds no read/write attempts. Reject
invalid lengths/masks during admission. Like pauses, checks remain subject to
the whole-transaction deadline and normal cleanup.

### SPI response polling

`spi::poll_response(window, expected, mask=0xff, fill=0xff)` repeatedly reads
a borrowed 1–32-byte window and compares its final byte under the mask.
CS and controller ownership remain held. Reject malformed windows/masks and
polling without an explicit transaction timeout before hardware activity.
Use the original whole-transaction deadline, never a new deadline per window.

The controller translates each window into an ordinary backend read. Backend
errors abort immediately; a nonmatching response requests another read.
Each window counts as a read attempt, while the logical action completes only
once after a match. A timeout leaves the polling action incomplete and performs
normal cleanup before the completion callback. No caller predicate or callback
executes between windows. This is protocol polling, not automatic retry of
failed transactions. IRQ processing retains the existing bounded work budget.

`spi::read_until(response, idle=0xff, maximum_bytes=0)` is the byte-preserving
variant: require a one-byte response span and clock until that byte differs
from idle. Retain the response; do not consume later payload bytes. A nonzero
32-bit byte limit returns `response_mismatch` when exhausted. With zero, only
the required explicit transaction timeout bounds the search. Ownership,
attempt accounting, bounded IRQ work and cleanup follow `poll_response`.

### SPI initialization clocks with CS inactive

Provide a dedicated SPI operation type for generating clocks while CS remains
inactive. It must retain exclusive controller ownership and use the normal
timeout, error cleanup, and callback-completion contract. Name the operation
`spi::idle_clocks(count)`, where `count` is the number of clock cycles, such as
`spi::idle_clocks(80)` for SD startup. Require a positive multiple of eight,
matching the initial 8-bit transfer support. Transmit ones on MOSI and discard
received data, keeping CS inactive throughout.

The operation runs alone as its own transaction; it cannot be mixed with
ordinary selected-device actions. Reject invalid counts or mixed action lists
before hardware activity, with no completion callback. Its wire time is
included in automatic timeout estimation.

The SD Association's [Physical Layer Simplified Specification, section
6.4.1.1](https://www.sdcard.org/cms/wp-content/themes/sdcard-org/dl.php?f=Part1_Physical_Layer_Simplified_Specification_Ver5.10.pdf)
requires at least 74 startup clocks with CMD/MOSI and CS high, allowing
preparation before the first command. This is power-up preparation before
CMD0 selects SPI operation, not an ordinary selected-device transaction.
ST's SD SPI reference driver sends ten `0xFF` bytes with CS high during
`SD_IO_Init()` in
`platform/stm32h7/STM32CubeH7/Drivers/BSP/Adafruit_Shield/adafruit_802_sd.c`.

### I2C transactions and addressing

Use an ordered action list for a single transaction, with controller ownership
retained throughout and one completion callback. Each read/write action starts
a new addressed phase: START for the first action, repeated START between
actions, and STOP at the end. A write followed by a read issues START,
address/write and payload, repeated START, address/read and reception, then
STOP. The attached-device configuration supplies the bus address.

Consecutive writes remain separate addressed phases, as do consecutive reads.
Callers wanting one continuous write provide one buffer. The I2C API has read and write data actions plus a standalone address-only
`probe()` action; pauses are not supported. A probe sends the write address
and STOP without a data byte, completing with ACK (`ok`) or `nack`. It cannot
be combined with other actions.

Payloads are arbitrary bytes; the HAL has no register-address interpretation.
A device driver may include a register address in its write buffer when its
protocol requires one. Devices needing direct reads or command/payload writes
without register addresses use the same API.

The initial API supports 7-bit I2C device addresses only, supplied as ordinary
**unshifted** values, such as `0x50`. Do not shift the address left or include
the read/write bit. The backend performs any encoding required by its vendor
HAL. Accept only `0x08` through `0x77`, inclusive; reject reserved addresses
`0x00` through `0x07` and `0x78` through `0x7F`, and all values outside the
7-bit range, during configuration validation. Ten-bit addressing is outside
the initial scope.

The public configuration header must document this convention beside the
address field/type: "Unshifted 7-bit I2C address, valid range 0x08–0x77 inclusive;
do not include the R/W bit or pre-shift for a vendor HAL." API examples must
use the same unshifted representation.

### Submission validation

Validate the entire action list before starting any hardware operation. Reject
an empty list, a zero-length transfer, or a zero-duration pause with
`invalid_argument`. Invalid submissions do not assert CS, execute earlier
actions, or invoke a completion callback. SPI exchange buffer constraints are
part of this validation.

Use `std::chrono` durations for pauses and transaction/reset timeouts. Reject
explicit zero or negative durations and durations that cannot be represented
exactly by the HAL's microsecond timebase, including overflow or fractional
microseconds. Omission of a timeout selects the automatic/default policy;
an explicit zero does not select that policy.

### Injected timing

Inject timer operations for transaction timeouts and SPI pauses. The bus HAL
does not depend directly on the scheduler. Applications may supply an adapter
backed by DaveOS timers; tests may supply a fake timer adapter. Timing adapters
must support the HAL's interrupt-context completion contract, including
simulated interrupt context in tests.

With the DaveOS timer adapter, transaction submission requires the scheduler
to be running, because DaveOS timers are unavailable during module
initialization. HAL initialization may configure controllers and prepare
handles; device probing and other startup transactions begin from scheduled
startup tasks after dispatch starts. An independently supplied timer backend
may permit earlier transfers. Adopt this initialization model initially and
revisit it if device bring-up demonstrates a need for a different arrangement.

Arm transaction timeout protection before accepting the request or starting bus
activity. If the timer backend cannot arm it, reject `start()` immediately with
an error, without asserting CS, starting a transfer, or invoking a completion
callback. Release any ownership acquired during submission. Every accepted
transaction must have timeout protection.

If a timer operation fails after acceptance, such as arming the end of a pause
or rearming an alarm, abort the transaction safely and deliver exactly one
completion with `timer_error`. If cleanup cannot restore usable idle, leave
the controller faulted until explicit reset. Do not continue transfer execution
without timeout protection. Hardware access to borrowed storage must stop
before returning it through the callback, as for every other failure.

Budget up to two simultaneously pending timers per active controller: one for
the overall deadline and one for SPI pauses or cleanup steps. Keep deadline
protection armed while a pause completes. Timers belong to the controller,
not to each attached device. With the DaveOS adapter, these use the existing
timer pool; applications size that pool for concurrently active controllers
plus other timer users. This budget is not a reservation or a guarantee that
arming succeeds; the failure rules above still apply.

Before releasing controller ownership and invoking completion, retire the old
operation's timers and ensure stale timer callbacks cannot affect a subsequent
operation. A completion callback may immediately reuse the controller and its
timer capacity. The injected clock exposes `now()`, `arm(absolute_deadline, callback)`, and
`cancel(callback)`. A selected callback may outlive cancellation. Alarm callbacks
carry no saved transfer outcome: they service current controller state and
reevaluate current absolute deadlines, so an old notification cannot complete
a replacement request early. Controller/timer IRQs share a serialized interrupt
domain, and dependencies outlive all in-flight callbacks.

### Transaction timeout

A caller may supply a finite transaction timeout. When omitted, the HAL
calculates a finite timeout using the configured bus speed and transaction
size. The timeout covers the entire accepted transaction, including all actions
and SPI pauses, rather than restarting for each action. Omission requests an
automatic timeout, not an unlimited wait.

Measure the timeout from transaction acceptance, including controller setup,
transfers, and pauses. Expiry begins abort/cleanup; completion may be delivered
after the deadline once hardware has stopped accessing borrowed storage. The
deadline bounds transfer execution, not the exact callback-delivery time.

Use one shared automatic-timeout estimation policy, based on the configured
speed and the transaction's actions, including explicit pauses. Do not expose
per-device safety-factor, margin, or minimum-timeout settings in the initial
API. Callers supply a per-transaction override for unusual timing requirements.
The default estimate is:

```text
sum(explicit pauses) + max(10 ms, 4 * estimated wire time + 1 ms)
```

Estimate wire time using the effective bus rate, including I2C address and
acknowledgment bits and SPI idle clocks. Round estimated wire time up to whole
microseconds. Use checked arithmetic for the estimate and absolute deadline;
reject unrepresentable requests before acceptance or bus activity. Keep the
factor, margin, and minimum in named constants. These are initial defaults,
not a guarantee about device response time: unusually slow devices or long
I2C clock stretching require a caller-supplied timeout. Exact conservative
accounting for protocol timing overhead remains an implementation detail.

On expiry, stop the operation and release controller ownership and borrowed
storage before delivering the single completion callback with `timeout`.
Hardware/DMA must no longer access the buffers when the callback begins, as
with any other completion. Timeout does not permit starting another transfer
on hardware that is still active.

The initial STM32 IRQ backends stop buffer access with a bounded RCC reset
sequence, requiring no additional alarm or wait for vendor abort completion.

There is no caller-facing cancellation API in the initial version. An accepted
transaction completes successfully, fails, or times out; a blocked transaction
is bounded by its timeout. The caller must preserve borrowed storage until its
completion callback even if the application no longer needs the result.
Internal timeout/error cleanup still stops hardware access before completion.

### Error cleanup and controller reset

On timeout or hardware error, abort the transfer and attempt to restore the
controller to idle. If recovery succeeds, release ownership before delivering
completion, allowing the callback to submit another transaction immediately.

If recovery fails, mark the controller faulted and reject new transfers until
an explicit `reset()` succeeds. Releasing transfer ownership does not make
a faulted controller available for new work. The completion callback still
receives the original transaction references, but hardware/DMA access to that
storage must have stopped before it is invoked.

Preserve the original transaction failure in its completion status even if
cleanup also fails: a timed-out transaction still reports `timeout`, for
example. Record the cleanup failure separately in controller diagnostics and
publish the faulted controller state before invoking the callback. Subsequent
transfers are rejected until explicit reset succeeds. Cleanup failure must
never permit completion while hardware can still access borrowed storage.

Explicit `reset()` is asynchronous: return acceptance or an immediate error
without waiting, then report an accepted reset's outcome through one completion
callback. A rejected reset request invokes no callback. The controller remains
unavailable for transfers while reset is in progress. Successful reset makes it
available before invoking the callback; failed reset leaves it faulted.

Allow explicit reset of a healthy idle controller as well as a faulted one.
If a transaction or another reset is active, return `busy` without disturbing
that operation or invoking the rejected reset's callback. Reset is not a
transaction cancellation mechanism.

The intended `reset()` contract permits submission from task or interrupt
context, with nonblocking acquisition of controller protection. An accepted
reset invokes exactly one interrupt-context completion callback. Use a finite
shared default reset timeout of 100 ms, expressed as a named constant, with an
optional caller-supplied override.

Interrupt-context reset submission is a desired convenience, not an essential
initial requirement. If backend constraints make it disproportionately complex,
report the concrete difficulty to the user and revisit this contract before
adopting a restriction; do not silently introduce blocking reset behavior.

Reset restores one controller to a known, usable idle state while preserving
registered devices and configuration. It may stop DMA/interrupt activity,
reset and reinitialize the peripheral, and return SPI chip selects to inactive.
An I2C backend may additionally attempt bus release; failure to restore usable
idle is reported as reset failure. Reset does not reinitialize attached devices,
replay their application-level setup, or retry a failed transaction. Those
operations remain the application/device driver's responsibility.

Controller reset preserves statistics. Only `clear_statistics()` clears them.

`Controller::reset(Callback<ResetResult>, optional<Duration>)` schedules reset
work in the bus interrupt. The STM32 I2C backend optionally accepts injected
open-drain GPIO recovery pins. Its asynchronous reset path clocks SCL up to
nine times while SDA is held low, then attempts STOP and checks both lines.
Each phase has at least 5 us dwell and is driven by timer ticks, not a busy
wait. SCL must actually rise before a high phase begins. The original reset
deadline bounds a held-low clock; failure leaves the controller faulted.
Timeouts and timer failures release GPIO and restore alternate functions.
Bounded peripheral cleanup/RCC reset hooks themselves never perform the pulse
sequence or retry a transaction. Backends without recovery pins retain the
peripheral-only reset plus idle check.

A recovering-capable backend can initialize with a low bus line, exposing
`needs_reset()` so the controller begins faulted. The command adapter defers
startup reset until scheduler dispatch starts; other HAL users explicitly reset
a faulted controller. No recovery timer is started during initialization.

### Borrowed transaction storage

The caller owns and maintains the action list and payload buffers for the
transaction's lifetime. The HAL borrows references without heap allocation or
internal payload copying. Until the completion callback begins, the caller must
not modify the descriptors or TX data, or access RX data. Referenced storage
must remain valid throughout the transfer and any pauses.

Before callback invocation, the HAL must have stopped accessing the completed
transaction's storage, including hardware/DMA accesses. The callback receives
the original references and may inspect the result, modify the descriptors or
buffers, and reuse them immediately in a new `start()`. The old completion path
must not access that storage after handing it back. A newly accepted transaction
establishes a new borrowing period under the same rules.

The polling helper must preserve the returned references and result and safely
publish completion before its task-context caller may reclaim or reuse storage.

### Transfer statistics

Count read/write attempts at action start, not at transaction submission.
Increment the corresponding error counter if that action fails or times out.
An SPI exchange counts as both a read and a write; a failed exchange increments
both error counters. Actions never reached after an earlier failure contribute
no read/write attempts or errors.

Track rejected submissions, including `busy`, separately from action statistics.
Pauses contribute no read/write attempts or errors. A timeout during a pause
still completes the transaction with `timeout` and contributes to transaction
failure and timeout counters.

Track accepted, completed, failed, timed-out, and rejected transactions per
device and per controller. Completed counts every accepted transaction whose
completion is delivered, including failures; failed is a subset of completed,
and timed-out is a subset of failed. Rejected requests never count as accepted
or completed. Publish completion counters before invoking the callback, so
task-context work notified by completion can observe the result in statistics.
Track cleanup failures and reset
outcomes at controller level, separately from transaction counters.

Maintain these statistics per attached device and make controller-wide totals
available. Use 64-bit saturating counters and provide synchronized,
task-context-only snapshots and `clear_statistics()` without disturbing active
transfers. Snapshot fields are synchronized samples, not a single atomic
multi-counter epoch. Internal counter updates still occur safely from interrupt context;
the restriction applies to public statistics access, not event accounting.
An operation spanning a clear may contribute a completion/error after its
earlier attempt was cleared; counters do not imply matched cohorts across a
clear. Device snapshots and clears apply only to that device; controller
snapshots and clears apply only to controller counters. Clearing either leaves
the other untouched. Controller totals are independently accumulated, not
computed by summing device snapshots, which may have different clear times.
Device lookup errors are compile-time errors rather than runtime transfer
attempts.

### Initial implementation scope

Start with deterministic fake SPI/I2C backends and interrupt-driven STM32H563
and STM32H755 backends. Keep the portable transaction and completion contracts
independent of the transfer mechanism.

The subsequent H755 phase adds optional SPI1 DMA for SD payloads through an
injected engine with private AXI staging, preserving caller-buffer ownership
and DTCM compatibility. H563 adds GPDMA1 channels 1/2 with private SRAM
staging through the same engine contract. No I2C DMA implementation
is planned; revisit only if an actual workload justifies it. The DMA buffer and
cache contracts above apply.

### Fake-backend validation

Provide deterministic scripted SPI/I2C fake backends. Tests specify expected
transaction actions and transmitted bytes, supplied receive bytes, completion
timing, and injected errors or hangs. Record chip-select transitions and
controller ownership so tests can verify pauses, contention, timeout cleanup,
and callback chaining. Preserve the simulated interrupt-context and borrowed
storage contracts.

The initial fake scope tests the HAL contract without emulating a complete SD
card or display. Full device-protocol models are not required for this phase.

### Implementation and validation

- `hal/` owns shared statuses, borrowed handles, actions, exact duration
  conversion, callback/polling completion, alias registries, and per-controller
  state machines. `daveos-hal` is the Meson dependency. The separate
  `hal/adapters/daveos.hpp` header supplies the scheduler timer adapter.
- `Controller::bind<Index>(controller)` takes an address without accessing an
  unconstructed dependency. Instance `device<Index>()` requires a live object.
  Bus registries validate all entries before setup and roll back initialized
  controllers if a later initialization fails. Registry initialization runs
  before application transfers are allowed.
- The shared CRTP machine performs all transitions through its `Step` switch.
  Public submissions publish requests; the bus interrupt drains bounded work.
  Application completion is delivered only after the tick returns and internal
  protection is released, allowing immediate transfer chaining.
- Host/fake controllers use independent try-lock domains. MCU controllers mask
  interrupts briefly instead of keeping a mutex held across hardware work.
  Counter storage uses lock-free 64-bit atomics on hosts and short masked
  accesses on 32-bit MCUs, avoiding allocating or blocking libatomic helpers.
- `platform/fake/bus.hpp` provides fixed-storage borrowed scripts, traces, and
  selectable timer notifications for race testing. Scripts can hang or fail an
  action. The test controls when the simulated IRQ runs.
- H5/H7 backends use common SPI/I2C register layouts through their own CMSIS
  headers. There are no global active-controller pointers or vendor callback
  singletons. Explicit board IRQ wrappers route to application-owned instances.
  The SPI backend supports fill/discard without transfer-sized scratch buffers.
  I2C issues START for every action and uses hardware reload within long actions.
- Vendor SPI abort functions poll hardware even in their `_IT` variants. These
  backends instead disable transfer interrupts and reset the peripheral in
  bounded register operations. GPIO CS becomes inactive before completion.
  No DMA bus master retains caller buffers. I2C line-level checks distinguish
  a usable bus from a reset controller whose slave still holds the bus low.
- H563 `spi_sd_probe` is an optional, read-only fixture. Five full inspections
  passed: startup/OCR, CSD/CID and 60 CRC-checked sector reads across 250 kHz
  and 1 MHz, with matching data and healthy watchdog/fault checks. The connected
  card has an MBR FAT32 partition; the probe checks its BPB without mounting.
  This is not filesystem consistency testing or qualification of every SPI
  mode. H563 I2C1 now has MCP3425 scan/conversion coverage (see below);
  Both boards now have SD/FatFs read/write and RX/TX DMA hardware coverage;
  H755 I2C remains build-only. See [SD inspection](../spi-i2c.md#nucleo-sd-fixture-and-validation).
- The optional [FatFs worker](../storage.md) uses the initialized card
  through an injected SD transport. Incremental response/token reads use a
  516-byte capture buffer. H755 DMA1 stream 1/2 stage payloads in two aligned
  512-byte AXI buffers, leaving UART stream 0 independent. H563 GPDMA1
  channels 1/2 use two private 512-byte SRAM buffers, leaving UART channel 0
  independent. Commands and tails still use interrupts on both boards.
  Both DMA directions and SPI must complete before RX is copied back. Failed cleanup faults the controller without retaining caller
  buffers. Multiblock transfers, higher speeds, injected DMA faults and wider
  bus/device qualification remain separate work.

## Injected flash and host file backend

Boot policy and the OTA engine receive an injected `boot::Flash` interface.
Host/fake simulations can use `platform::host::FileFlash`, a raw persistent file
with caller-supplied geometry and an optional injected clock. It models bounds,
sector erase, aligned 16- or 32-byte programming (16 by default), and one-to-zero NOR bits, with fsync
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

### Execution context for task yielding

`core::Context` carries borrowed module/task names, `CallbackKind`, and the
owning scheduler identity. Context must be local to the executing thread on
host platforms, with scope guards restoring the previous value. Host/fake use
thread-local storage. STM32 checks exception state separately, so an interrupt
cannot inherit a preempted task's permission to yield. This is API misuse
checking, not a security boundary; applications must not forge context values.
See [task yielding](../yield.md).

### H563 I2C ADC diagnostic fixture

The optional `i2c_adc_probe` example component uses I2C1 on PB8/PB9 and an
MCP3425 at seven-bit address 0x68. Its portable reader borrows a HAL device,
performs nonblocking 16-bit gain-1 one-shot conversions, checks configuration
readback, and preserves transfer-buffer ownership on timeout. The fixture also
provides separate `i2c` and `adc` modules: `i2c scan` prints a serialized
16-column by 8-row address map for each configured bus, `i2c stats` reports
named counters for all configured buses, and `adc sample` requests a conversion. The ADC holds an I2C lease across the
conversion so diagnostics cannot interrupt its transaction sequence.
`Controller::probe(address, callback, timeout)` copies the target address
without mutating any registered device, and records only controller counters.
A standalone I2C `probe` action sends the write address followed by STOP, with no payload; only ACK and
NACK classify presence. Reserved addresses are excluded. Probe transactions
contribute to transaction/error statistics, not read/write action counters.
See [SPI/I2C](../spi-i2c.md) for commands, wiring, timing and validation limits.

The I2C module also provides `i2c reset` with per-bus outcomes. Statistics remain
64-bit; text output saturates each overflowing field to `4294967295+` instead
of silently truncating. Peripheral drivers live in the separate `drivers/`
component and `daveos-drivers` dependency; MCP3425 is the first driver.

### SPI/SD timeout policy

SPI always performs bounded cleanup and returns buffers before completion;
failed cleanup latches the controller fault until an explicit successful reset.
SD separately invalidates its initialized-card session after an accepted I/O
failure or abandoned protocol sequence. Successful SPI cleanup/reset does not
clear that condition. Recovery and retries belong to application policy, with
no automatic command replay, card reinitialization, or power cycle. Failed
write outcomes are uncertain. See [storage recovery](../storage.md#failure-and-explicit-recovery)
for the example's unmount, controller-reset, probe and remount sequence.
