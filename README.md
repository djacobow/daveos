# DaveOS

A C++20 cooperative scheduler for embedded applications. [PROJECT.md](PROJECT.md)
is the behavioral specification. The initial implementation supplies real-time
Linux host and deterministic fake-time platforms, plus an STM32H563 adapter and
a CubeMX-based DaveOS LED example.

## Build and run

Install Meson (1.3 or newer), Ninja, clang++, Python 3, clang-format 15, and
cppcheck. The first test-enabled setup downloads Catch2 3.16.0 and verifies its
SHA-256. It is a test-only dependency. Python helpers use only the standard library;
there are no Python package dependencies yet.

```sh
meson setup build/host --native-file meson/clang.ini
meson compile -C build/host
meson test -C build/host --print-errorlogs
./build/host/examples/hello/hello-host
./build/host/examples/system/system-host
```

The host configuration also builds fake-time tests and examples. To build only the
fake adapter:

```sh
meson setup build/fake --native-file meson/clang.ini -Dplatform=fake
meson compile -C build/fake
meson test -C build/fake --print-errorlogs
./build/fake/examples/system/system-fake
```

`-Dtests=false` omits Catch2 and unit/integration test executables.
`-Dexamples=false` omits example executables. Meson suites are `smoke`, `unit`, and
`integration`; use `meson test -C build/host --suite unit` to select one.

All generated files, downloaded test sources, and analysis caches live under the
ignored `build/` directory. It can be removed entirely and regenerated. Installed
toolchains in `tools/external/` are separately ignored inputs, not build outputs.

## Build structure

Build definitions follow the dependency and target directories:

- `src/core/meson.build`: allocation-free core headers.
- `src/platform/{host,fake,stm32h5}/meson.build`: reusable adapter libraries.
- `platform/stm32h5/{cmsis,hal}/meson.build`: vendor headers, device flags,
  and HAL component source dependencies.
- `examples/{hello,system,console,stm32h563_blinky}/meson.build`: application targets
  that select dependencies and supply their own configuration.
- `tests/catch2/meson.build`: test framework dependency; other test directories
  define their respective test executables.
- `tools/meson.build`: formatting and lint targets.

The root `meson.build` selects the platform and includes these groups. HAL sources
compile separately for each firmware target, using that target's
`stm32h5xx_hal_conf.h`. Host/fake configurations do not require ST submodules;
STM32 configurations require CMSIS even when examples are disabled.

Example and test binaries live in their corresponding directories under `build/`.
Use `meson test -C build/host` to run tests without depending on binary paths.

## Formatting and linting

```sh
meson compile -C build/host format       # Rewrite project C++ using Google style.
meson compile -C build/host format-check # Check without editing.
meson compile -C build/host lint         # cppcheck; diagnostics fail the target.
```

These targets are available in every configuration. They also work without Meson:
`python3 tools/check.py format-check` and `python3 tools/check.py lint`.
The helper prefers `clang-format-15`, falling back to `clang-format`. Lint checks
production code and examples, including both platform configurations. The
`duplInheritedMember` diagnostic is suppressed because CRTP intentionally hides
inherited defaults; other enabled warning, performance, and portability checks
remain active. Test and downloaded framework sources are excluded from cppcheck.
CubeMX-generated `Core/` files and copied `Drivers/` are excluded from both
formatting and linting; regeneration preserves ST's formatting.

## ARM compile check

STM32CubeH5 is pinned as a Git submodule at
`platform/stm32h5/STM32CubeH5`. Initialize its HAL and CMSIS device dependencies
after cloning DaveOS:

```sh
git submodule update --init platform/stm32h5/STM32CubeH5
git -C platform/stm32h5/STM32CubeH5 submodule update --init --recursive \
  Drivers/CMSIS/Device/ST/STM32H5xx Drivers/STM32H5xx_HAL_Driver
```

These commands omit the board BSP and middleware submodules. The
STM32H563 example uses CubeMX-generated application configuration with these
external HAL/CMSIS sources. Vendor sources live outside the project formatting
and linting paths.

Put `arm-none-eabi-g++`, `arm-none-eabi-ar`, and the companion tools on `PATH`.
For the locally installed toolchain:

```sh
export PATH="$PWD/tools/external/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi/bin:$PATH"
meson setup build/arm --cross-file meson/stm32h563.ini
meson compile -C build/arm
```

This builds a Cortex-M33 static archive that instantiates the scheduler and queue
APIs without host dependencies. The STM32 adapter library is also built, even with examples disabled.
Neither static library is a firmware image.
The cross-file uses the Cortex-M33 FPv5 single-precision hard-float ABI for both
C and C++, matching the generated CubeMX toolchain.

To also build the DaveOS LED firmware:

```sh
meson setup build/arm --cross-file meson/stm32h563.ini -Dexamples=true
meson compile -C build/arm
```

Use `--wipe` with setup when replacing an existing build configured with the old
cross-file flags. ELF, HEX, BIN, and map files are written under
`build/arm/examples/stm32h563_blinky/`. A repeating DaveOS task toggles LD1 (PB0)
every 500 ms, giving a one-second blink cycle. The firmware has been
cross-compiled, but not tested on hardware.

The CubeMX source project is `examples/stm32h563_blinky/blinky_demo.ioc`, selecting
STM32H563ZIT6. Keep generated Core sources, the startup assembly, and the FLASH
linker script in Git. Copied drivers and generated CMake files are ignored;
Meson owns the build. After CubeMX regeneration, update the example's HAL dependency
selection if enabled peripherals change. Run builds through Meson to keep all outputs
under the repository's `build/` directory.

The generated `main.c` calls `DaveOS_Run()` from a CubeMX USER CODE section after
peripheral initialization. The application and IRQ bridge live in `blinky.cc`.
The scheduler lives on the main stack; the `.ioc` and FLASH linker script reserve
16 KiB for it and interrupt frames. GCC's `.su` stack reports are emitted beside
the example's object files. C++ exceptions and RTTI are disabled. The example
allows hosted headers because ST's umbrella header includes `math.h`; the core
and STM32 adapter remain compiled in freestanding mode. The firmware disables
standard-library runtime assertions to avoid their allocating stdio error path;
host tests retain them.

`daveos::platform::stm32h5::Platform` owns TIM2 and compare channel 1. Call
`init(timer_kernel_hz)` after clock setup, then forward `TIM2_IRQHandler()` to
`interrupt()`. The frequency must divide exactly to 1 MHz. The example derives
it from the APB1 configuration with TIMPRE disabled. Reserve TIM2 in future
CubeMX configurations and leave the clock tree unchanged while DaveOS runs.

The 32-bit hardware count is extended to 64-bit microseconds with the overflow
interrupt. Long timers use intermediate compare deadlines. Critical sections
save/restore PRIMASK and can nest; do not mask interrupts for an entire counter
period (about 71 minutes), or call DaveOS from NMI/HardFault handlers. Logging
from an ISR receives the `core/interrupt` context automatically.

Idle uses shallow WFI sleep with TIM2's sleep clock enabled, or polls when a
module declines sleep. Deep sleep/Stop modes are not supported. The final idle
check and WFI run with interrupts masked to avoid a lost wakeup; pending enabled
interrupts wake the core before their handlers run, as described in
[Arm's power-management guidance](https://documentation-service.arm.com/static/5ef9ff27cafe527e86f55b47).
SysTick remains the HAL timebase and may wake the CPU every millisecond.
Embedded `stop()` is a no-op. Host register-model tests exercise the adapter's
timer and interrupt logic; hardware timing and sleep validation remain to do.

## Application structure

Modules use `daveos::core::Module<Derived, Event>`; platforms are statically bound.
There are no virtual methods in DaveOS. Modules receive a non-owning
`SchedulerInterface<Event>` reference backed by a fixed function-pointer table,
so module types do not depend on all other modules or scheduler capacities.

```cpp
using namespace daveos::core;
enum class Event { ready };

class Blinker : public Module<Blinker, Event> {
 public:
  static constexpr const char* name() { return "blinker"; }
  static constexpr auto tasks() {
    return std::array{TaskDescriptor<Blinker>{"tick", &Blinker::tick}};
  }
  Status init(InitStage stage) {
    if (stage == InitStage::stage1)
      return scheduler().schedule(*this, &Blinker::tick, 1000, Mode::repeat);
    return Status::ok;
  }
  void tick() {
    scheduler().log(Level::info, "tick");
    scheduler().cancel(*this, &Blinker::tick);
    scheduler().stop();
  }
};
```

Construct a platform and modules before the scheduler. Without logging, use
`make_scheduler<Event>(platform, ModuleList{&one, &two})`. The optional numeric
template arguments now specify only event and timer slots, defaulting to `32, 16`.
Task storage is inferred from module descriptors.

To attach logging, include `daveos/core/logger.h` and construct an application-owned
logger before the scheduler:

```cpp
auto logger = make_logger(platform, SubscriberList{Subscriber{nullptr, Output}});
auto scheduler = make_scheduler<Event>(platform, modules, logger);
```

The logger and scheduler must use the same platform. `make_logger<32, 128>` controls
record capacity and message bytes (including the terminating NUL); subscriber
storage is inferred from the list. The logger and subscriber contexts must outlive
the scheduler. Do not move an attached logger. The scheduler stores only a borrowed
attachment, not the buffers. Use `logger.minimum(Level::debug)`,
`logger.counters()`, and `logger.reset()` for logging configuration and diagnostics.
Scheduler snapshots/resets cover task, event, and timer statistics separately.

See [hello.cc](examples/hello/hello.cc) for a complete program and
[system.cc](examples/system/system.cc) for repeating work, events, interrupt timers,
deferred task execution, and statistics output.

Modules, the platform, subscriber contexts, and module/task name strings must
outlive the scheduler. Do not move a bound module or scheduler. Each platform
instance serves one scheduler run; shutdown closes its interrupt domain and timer.
Run returns only after timer activity has stopped and queued logs have been flushed.
External producer threads must finish before destroying the platform. Log record
views are valid during the subscriber call; copy data needed for later output.
Subscribers append line endings themselves, as shown in the examples.
On GCC and Clang, `SchedulerInterface::log()` checks literal printf format strings
against their arguments at compile time. Test configuration verifies that valid
arguments compile and mismatched types fail with `-Werror=format`.

Module member functions can use `D_`, `I_`, `W_`, `E_`, and `F_` from
`daveos/core/log.h` for debug, info, warning, error, and fatal messages:

```cpp
I_("started");
W_("retry %u", retry_count);
```

With logging enabled, these macros call `this->scheduler().log(...)` and return
its `Status`. They preserve format checking, evaluate arguments once, and use
buffered, line-oriented logging. `F_` only selects severity; it does not halt execution.
Outside module member functions, use `DAVEOS_LOG(scheduler, Level::info, ...)`
for the same compile-time elision, or call `scheduler.log(...)` directly.

Logging is enabled by default and independent of commands. To build without it:

```sh
meson setup build/no-logging --native-file meson/clang.ini -Dlogging=false
meson test -C build/no-logging --print-errorlogs
```

In that configuration, the macros return `Status::ok` without evaluating any
arguments. Keep program state changes outside logging arguments. Direct `.log()`
calls also return `ok`, but C++ still evaluates their arguments. There are no log
buffers, formatting, or scheduler drain/flush hooks; the logger type becomes empty,
so applications can retain the same construction code. Without Meson, consistently
define `DAVEOS_LOGGING=0` across all translation units (default is `1`).

An enabled build can also omit the logger attachment; calls then discard output
successfully. An attached logger delivers one record during idle time before the
scheduler checks due work again. Pending output prevents sleep, and enqueueing
notifies the platform, including from interrupts. Init failure and shutdown flush
remaining records. Logging counters and resets belong solely to the logger.

Commands run in either build: without logging, help, echo, and diagnostics are
silent, but handlers and their status results still work, including `console exit`.
Neither facility requires the other.

`Queue<T, N>` provides ordinary fixed storage. `ThreadSafeQueue<T, N, Platform>`
uses a suitable platform mutex or falls back to critical sections. All of its
operations are nonblocking. Mutations and `peek(out)` return `Status`; state
queries return `Result<T>` so a busy lock cannot be mistaken for an empty queue.
Elements must support default construction and assignment without allocation or
exceptions; interrupt callers must also keep element operations bounded.

`timer(delay, callback)` and `cancel_timer(callback)` use `void (*)()` callbacks.
Callbacks are identified by pointer equality and execute in interrupt context;
use wrapper functions for separate timers. Zero delays are rejected. Pre-run timer
requests return `not_running`; pre-run *task* schedules are retained instead.

## Commands

Include `daveos/core/command.h` and expose a constexpr descriptor array:

```cpp
class Motor : public Module<Motor, Event> {
 public:
  static constexpr const char* name() { return "motor"; }
  static constexpr auto commands() {
    return std::array{
        DAVEOS_COMMAND(Motor, "speed", SetSpeed, "Set motor speed")};
  }
  Status SetSpeed(CommandArguments args) {
    if (args.size() != 1) return Status::invalid_argument;
    I_("requested speed: %.*s", static_cast<int>(args[0].size()), args[0].data());
    return Status::ok;
  }
};
```

`DAVEOS_COMMAND` captures `SetSpeed` as both the member-function pointer and its
logging name. Logs from this handler identify `motor/SetSpeed`. Task and command
arrays default to empty; command-only modules need no task. Module names are now
static constexpr accessors, not constructor arguments. Modules with different
instance names must use different template instantiations. Names must be nonempty
and unique ignoring ASCII case; registration checks this at compile time.

Construct `CommandDispatcher dispatcher(modules, scheduler)` using the same
`ModuleList` supplied to the scheduler. Call `dispatcher.dispatch(line)` from a
normal scheduled task or event callback, after initialization. The application
collects complete lines and serializes inputs. Interrupt-time dispatch is rejected,
and dispatch before `run()` or after shutdown returns `not_running`.

`motor speed 100` calls `SetSpeed` with just `100`. Matching ignores ASCII case:
an exact name wins, otherwise a unique prefix is accepted. Override
`static constexpr const char* command_prefix()` to route using another name.
Prefixes and command names allow ASCII letters, digits, `_`, and `-`; duplicates,
invalid names/callback metadata, and reserved `help` collisions fail compilation.

Double quotes group whole arguments, including empty arguments; mixed forms such
as `ab"cd"` are invalid. Backslash escapes quotes and backslashes; other sequences
remain literal. Handlers receive `CommandArguments`, a
`std::span<const std::string_view>` valid only until they return, and validate their
own arguments. Nested calls on the same dispatcher return `busy` and preserve
active views. Inputs need no terminating NUL; embedded NUL bytes are rejected.

Defaults are 256 input bytes and 16 arguments, counting the prefix and command.
To customize, use `CommandDispatcher<Event, decltype(modules), 512, 24>`.
Overflows reject the complete line without invoking a handler. Specific results
are `parse_error`, `ambiguous_match`, `line_too_long`, and `too_many_arguments`;
unknown names return `not_found`. Handler results propagate unchanged.

`help` lists the complete tree and short descriptions. `motor` or `motor help`
lists that module's commands; extra arguments to help are errors. All help and
errors use ordinary best-effort buffered logging under `core/command`. Output
buffer overflow and filtering apply just as for statistics tables; increase the
logger's capacity if the default cannot accommodate a full tree.

Run the host console example with:

```sh
./build/host/examples/console/console-host
# Try: help, console echo "Hello World", console exit
```

Its input thread assembles lines into a bounded queue; a scheduled task dispatches
them. `console exit` requests shutdown and log flushing. EOF only ends input
collection and does not stop the scheduler. UART transport integration is left
to the application and is not part of this example.

## Fake time and host interrupts

The fake platform defaults to automatic advancement. When idle it advances to the
next task or timer deadline. `Fake::Advancement::manual` lets a test advance time
with `advance(microseconds)` or inject an interrupt explicitly. Due timer callbacks
run synchronously in simulated interrupt context before advancement returns.
An indefinite wait requires an external simulated interrupt in either mode.

Both platforms expose `interrupt(callback, context)` to test/application adapter
threads. It executes the handler synchronously, serialized with other handlers.
Callbacks can schedule tasks, post events, operate timers, and log. They must not
call `run()` or directly dispatch module callbacks. Nested interrupt injection is
rejected. Host timer expiry uses a dedicated thread and the same interrupt domain.

The internal idle/notification hooks use a retained generation counter. They are
platform implementation details, not an application wake API. Real-time host
blocking is not an MCU power state: both sleep and awake-wait use host notifications.
Fake-time tests distinguish and exercise both scheduler paths.

## Sanitizers and CI

```sh
meson setup build/asan --native-file meson/clang.ini -Db_sanitize=address,undefined -Db_lundef=false
meson compile -C build/asan -j 4
meson test -C build/asan --print-errorlogs

meson setup build/tsan --native-file meson/clang.ini -Db_sanitize=thread -Db_lundef=false
meson compile -C build/tsan -j 4
meson test -C build/tsan --print-errorlogs
```

GitHub Actions runs host/fake tests, sanitizers, format/lint checks, and the ARM core
and LED firmware builds. Allocation tests instrument C++ `new` during representative core
operations in ordinary and ASan builds (TSan owns its own allocator interceptors);
they do not certify allocator behavior inside every platform libc
formatting implementation. ARM firmware must validate its chosen libc as well.

Remaining work is tracked in [TODO.md](TODO.md).
