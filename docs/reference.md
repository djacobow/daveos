# Build and API reference

A C++20 cooperative scheduler for embedded applications. [PROJECT.md](../PROJECT.md)
is the concept/core specification and index to the services, platforms, and
STM32 integration specifications. The implementation provides real-time Linux
host and deterministic fake-time platforms, STM32H563/H755 adapters, optional logging and command dispatch, and
standalone application starter files. One shared STM32 console supports both
Nucleo boards, with independently selectable UART, USB CDC, and TCP transports
and optional lwIP Ethernet networking.

Run these commands from the DaveOS repository root. For a guided introduction,
start with [the learning path](../README.md).

Optional FAT12/16/32 storage is enabled with `-Dfatfs=true`; see
[read-only storage](storage.md) for the injected block-device API and SD commands.

## Build and run

Install Meson (1.3 or newer), Ninja, clang++, Python 3, clang-format 15, and
cppcheck 2.19.0. CI builds this pinned version from a checksum-verified upstream
archive under `build/ci-tools/`; Ubuntu 24.04’s packaged 2.13 cannot expand
the C++20 `__VA_OPT__` used by our enum macros. The first test-enabled setup
downloads Catch2 3.16.0 and verifies its SHA-256. It is a test-only dependency. Python helpers use only the standard library;
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

## OTP storage

See [OTP records and host testing](otp.md) for the injected store, optional command
module, persistent file backend, and optional H563 bank-B flash emulator.
Real H563 OTP provisioning has passed one explicitly authorized write/lock
validation. Provisioning is disabled by default; automated hardware tests only
read OTP.

## Starting your own application

Copy [`starters/application`](../starters/application/README.md) into a new repository
for a small host application and a fake-time test of the same module. It consumes
DaveOS as a Meson subproject; no edits to the framework build are needed.
See the [application guide](application-guide.md) for task helpers, explicit
time units, periodic tasks, Application composition, and failure diagnostics.

## Build structure

Code is organized by component, with headers and implementations together:
`core/{schedule,command,event,logging,queue,platform,enum}/` and
`platform/{host,fake,stm32h5,stm32h7,detail}/`. Optional networking lives in
`net/` and console helpers in `console/`, with shared hardware support in `platform/stm32/ethernet/`.
There is no separate `include/`
or `src/` tree. Include paths start at the repository root, for example
`#include "core/schedule/scheduler.hpp"`. Namespaces are `daveos::core`,
`daveos::platform::*`, `daveos::console`, and `daveos::net` (including `daveos::net::stm32`).

Headers defining templates use `.hpp`; other headers use `.h`. C++ translation
units use `.cpp`. Generated and third-party files retain their supplied names.
The shared STM32 template declarations and CMSIS-dependent definitions live
side by side in `platform/detail/stm32_tim2.hpp` and `stm32_tim2_impl.hpp`.

Build definitions follow the dependency and target directories:

- `core/meson.build`: allocation-free core headers.
- `platform/{host,fake,stm32h5,stm32h7}/meson.build`: reusable adapter libraries.
- `platform/{stm32h5,stm32h7}/{cmsis,hal}/meson.build`: vendor headers, device flags,
  and HAL component source dependencies.
- `examples/{hello,system,console,stm32_console}/meson.build`: application targets
  that select dependencies and supply their own configuration.
- `tests/catch2/meson.build`: test framework dependency; other test directories
  define their respective test executables.
- `tools/meson.build`: formatting and lint targets.

The root `meson.build` selects the platform and includes these groups. HAL sources
compile separately for each firmware target, using that target's
`stm32h5xx_hal_conf.h` or `stm32h7xx_hal_conf.h`. Host/fake configurations do not require ST submodules;
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
Formatting uses Google style with blank lines between function and class
definitions (`SeparateDefinitionBlocks: Always`), including inline methods.
Namespace contents are indented (`NamespaceIndentation: All`). Control-flow
bodies require braces (`InsertBraces: true`); clang-format applies this to
`if`/`else` and loops, including single-statement bodies.
Use short namespace aliases (for example, `namespace core = daveos::core;`)
and qualified names instead of namespace-wide using directives.
The helper prefers `clang-format-15`, falling back to `clang-format`. Lint checks
production code, examples, and application starters, including both platform
configurations. The `duplInheritedMember` diagnostic is suppressed because CRTP intentionally hides
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
meson setup build/arm --cross-file meson/stm32.ini
meson compile -C build/arm
```

This builds a Cortex-M33 static archive that instantiates the scheduler, queue,
command dispatcher, and optional logger APIs without host dependencies. The
STM32 adapter library is also built, even with examples disabled.
Neither static library is a firmware image.
The shared `meson/stm32.ini` cross file selects the ARM toolchain and
`platform=stm32`. Meson selects CPU and hard-float flags from `board`: `h563`
(the default, Cortex-M33/FPv5 single precision) or `h755` (Cortex-M7/FPv5
double precision). The H755 sleeping M4 has its own Cortex-M4/FPv4 flags.
Board selection applies to the platform libraries as well as the application.

To also build the DaveOS board console firmware:

```sh
git submodule update --init platform/stm32/STM32_USB_Device_Library
# Enable firmware in the build configured above.
meson configure build/arm -Dexamples=true
meson compile -C build/arm
```

For an existing build made with the old board-specific cross files, reset its
saved compiler/linker options when migrating (Meson preserves them across a wipe):

```sh
meson setup --wipe build/arm --cross-file meson/stm32.ini \
  -Dplatform=stm32 -Dboard=h563 -Dexamples=true \
  -Dc_args= -Dcpp_args= -Dc_link_args= -Dcpp_link_args=
```

For a fresh firmware build, use
`meson setup build/arm --cross-file meson/stm32.ini -Dexamples=true`.
Both boards build the same `stm32-console` target. ELF, HEX, BIN, and map files
are written under `build/arm/examples/stm32_console/` as `stm32-console.*`.
The board-specific CubeMX files and peripheral glue live in
`platform/stm32/nucleo/{h563,h755}/`; their Meson files contribute
sources, dependencies, compiler definitions, and a linker script to the shared
target. Use `-Dboard=h755` with the same cross file for the other board.
Separate build directories are convenient, but `meson configure build/arm
-Dboard=h755` followed by a rebuild also switches the board (initialize its
vendor submodules first).
Both boards run the same console commands: `help`,
`board led <1|2|3> <on|off|toggle>`, `board button`, `board stats`,
`board timer <microseconds>`, `board version`, and `board reset`.
LEDs are PB0/PF4/PG4 and the button is PC13. USART3 uses PD8 TX / PD9 RX at
1,000,000 baud, 8N1, no flow control; disable terminal local echo.

RX uses the hardware FIFO, one-byte interrupts, and a 16-line queue. TX uses GPDMA1 Channel 0
with the USART3 TX request, normal memory-to-peripheral byte transfers, and
completion/error interrupts. Two 4 KiB ping-pong buffers live in normal SRAM;
GPDMA and SRAM clocks remain enabled during shallow sleep. Echo, line-oriented
logs, whole-frame overflow drops, DMA statistics, and immediate reset match H755.
The internal 64 MHz HSI drives PLL1 (M=16/N=125/P=2), giving nominal 250 MHz CPU,
62.5 MHz PCLK1, and a 125 MHz TIM2 kernel divided down to 1 MHz. No external
crystal is required. Both console targets retain full newlib; statistics use integer decimal formatting
to avoid its allocating floating-point formatting path.
H563 also provides the same independent USB CDC command/log transport on
**CN13 (USB Type-C)**, using PA11/PA12 and the USB DRD FS controller. Keep ST-LINK
connected for power/debugging and connect CN13 to a USB host with a data cable.
The Linux device identifies as `usb-DaveOS_DaveOS_H563_console_*-if00`.
Open with DTR asserted and terminal local echo disabled; USB baud settings are
ignored. Both `uart_console` and `usb_console` options apply, allowing either,
both, or neither transport, independently of logging.

USB uses HSI48 with CRS synchronized to USB SOF, IRQ priority 6, and five
single-buffer endpoint allocations in packet memory (PMA). Transfers are
interrupt-driven, without DMA. The board glue retains the reset-state USB-C
sink terminations through UCPD dead-battery mode; no Power Delivery stack or
source power switch is enabled. Keep PA9/PA10 and PB13/PB14 reserved for the
board's Type-C attachment circuitry. See ST's
[NUCLEO-H563ZI manual](https://www.st.com/resource/en/user_manual/dm00936683.pdf).

Unlike H755 OTG FS, H563 DRD FS has no VBUS disconnect interrupt. Suspend clears
unfinished input and queued output; DTR is retained for normal host resume.
Bus reset and DTR deassertion also clear session buffers. Physical reconnect and USB-C
attachment in both cable orientations have passed hardware checks.
H563 hardware bring-up has verified HSI boot, UART/USB commands and logging,
TX DMA, all three LEDs, button press/release, the 200 ms timer command, DHCP,
large-packet ping, and the TCP console. Software reset recovers UART/USB and
DHCP/TCP operation.

The CubeMX source project is `platform/stm32/nucleo/h563/blinky_demo.ioc`, selecting
STM32H563ZIT6. Keep generated Core sources, the startup assembly, and the FLASH
linker script in Git. Copied drivers and generated CMake files are ignored;
Meson owns the build. After CubeMX regeneration, update the example's HAL dependency
selection if enabled peripherals change. Run builds through Meson to keep all outputs
under the repository's `build/` directory.

The generated `Core/Src/main.c` includes the C-compatible `appmain.h` and calls
`appmain()` from a CubeMX USER CODE section after peripheral initialization.
The shared `examples/stm32_console/appmain.cpp` instantiates the selected
components, logger, and Application at file scope and calls
`application.run()`. Application owns the scheduler and dispatcher.
Each target supplies `board_config.h` and `console.cpp`
for its platform, LED/button access, DMA storage/cache handling, and timer clock.
The modules, logger, scheduler, dispatcher, and transport buffers live at file
scope. Their constructors store references and metadata; UART/USB/network setup
runs in stage1. Application binds command sources after both initialization
stages succeed; `Board` only provides board commands.
The platform timer is initialized after CubeMX peripheral setup and before
scheduler initialization. The stack uses all main SRAM above static data, with MSPLIM guarding that
boundary. The linker checks at least 64 KiB of headroom rather than limiting the
stack to that size. Heap growth is disabled. See [STM32 memory](memory.md) for
allocation auditing and stack watermarking. GCC's `.su` reports are emitted
beside the example's object files. C++ exceptions and RTTI are disabled. The example
and the ARM core compile check allow hosted headers: ST's umbrella header
includes `math.h`, and GCC 13's `<chrono>` requires this mode. They still target
newlib without host/OS APIs; these headers do not themselves require allocation.
The STM32 adapter remains compiled in freestanding mode. The firmware disables
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
from an ISR receives the `core.interrupt` context automatically.

Idle uses shallow WFI sleep with TIM2's sleep clock enabled, or polls when a
module declines sleep. Deep sleep/Stop modes are not supported. The final idle
check and WFI run with interrupts masked to avoid a lost wakeup; pending enabled
interrupts wake the core before their handlers run, as described in
[Arm's power-management guidance](https://documentation-service.arm.com/static/5ef9ff27cafe527e86f55b47).
SysTick remains the HAL timebase and may wake the CPU every millisecond.
Embedded `stop()` is a no-op. Host register-model tests exercise the adapter's
timer and interrupt logic. Initial H563 hardware checks passed; precision timing,
extended sleep/wake behavior, and injected-error stress remain in TODO.md.

## Application structure

Modules use `daveos::core::Module<Derived, Event>`; platforms are statically bound.
There are no virtual methods in DaveOS. Modules receive a non-owning
`SchedulerInterface<Event>` reference backed by a fixed function-pointer table,
so module types do not depend on all other modules or scheduler capacities.

```cpp
namespace core = daveos::core;
struct Ready {};
using Event = std::variant<Ready>;

class Blinker : public core::Module<Blinker, Event> {
 public:
  static constexpr const char* name() { return "blinker"; }

  static constexpr auto tasks() {
    return std::array{DAVEOS_TASK(Blinker, tick)};
  }

  core::Status init(core::InitStage stage) {
    if (stage == core::InitStage::stage1) {
      return schedule<&Blinker::tick>(std::chrono::milliseconds{1}, core::Mode::repeat);
    }
    return core::Status::ok;
  }

  void tick() {
    scheduler().log(core::Level::info, "tick");
    (void)cancel<&Blinker::tick>();  // This example deliberately ignores cancellation status.
    scheduler().stop();
  }
};
```

Own the platform and modules for the scheduler's lifetime and complete their
construction before calling `init()` or `run()`. The scheduler constructor only
records references and static metadata; it does not initialize modules. Without
logging, use
`make_scheduler<Event>(platform, ModuleList{&one, &two})`. The optional numeric
template arguments now specify only event and timer slots, defaulting to `32, 16`.
Task storage is inferred from module descriptors.

To attach logging, include `core/logging/logger.hpp` and construct an application-owned
logger before the scheduler:

```cpp
auto logger = core::make_logger(
    platform, core::SubscriberList{core::Subscriber{nullptr, Output}});
auto scheduler = core::make_scheduler<Event>(platform, modules, logger);
```

The logger and scheduler must use the same platform.
Different platform types are rejected at compile time; different instances of the same type make scheduler `init()` return
`Status::invalid_argument` before any module initialization callback. This failure
is terminal and follows the normal initialization-failure cleanup and log flush.
There is no platform check when logging is compiled out.

`make_logger<32, 128>` controls
record capacity and message bytes (including the terminating NUL); subscriber
storage is inferred from the list. The logger and subscriber contexts must outlive
the scheduler. Do not move an attached logger. The scheduler stores only a borrowed
attachment, not the buffers. Use `logger.minimum(Level::debug)`,
`logger.counters()`, and `logger.reset()` for logging configuration and diagnostics.
Scheduler snapshots/resets cover task, event, and timer statistics separately.

See [hello.cpp](../examples/hello/hello.cpp) for a complete program and
[system.cpp](../examples/system/system.cpp) for repeating work, events, interrupt timers,
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
`core/logging/log.hpp` for debug, info, warning, error, and fatal messages:

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

`timer(delay, callback)` and `cancel_timer(callback)` accept `TimerCallback`:
a plain `void (*)()` function, a noncapturing lambda, or an object-bound member
callback. Plain functions use pointer identity; bound callbacks use the object
and member function. Modules can call `timer<&Type::Expired>(delay)` and
`cancel_timer<&Type::Expired>()` without a global owner pointer. Separate objects
can use the same member independently; repeated requests for one pair replace
its pending timer. Bound objects must outlive pending and in-flight callbacks.
Callback identity relies on distinct function addresses. Do not enable unsafe
linker folding such as `--icf=all`; this also affects plain function callbacks.
The supplied host and ARM builds do not request that option.

Timers still execute in interrupt context. Delays accept raw microseconds or
integral chrono durations; invalid conversions are rejected without changing
pending work. Zero timer delays are invalid. Valid pre-run timer requests return
`not_running`; pre-run *task* schedules are retained instead. See the
[application guide](application-guide.md) for examples and conversion rules.

`init()` and `run()` return statuses marked `[[nodiscard]]`.
`initialization_failure()` retains the first failure's status, module, and stage,
including with logging disabled; a null module identifies registration validation.
The STM32 application saves this snapshot and `app::last_status` before halting.

## Commands

Include `core/command/command.hpp` and expose a constexpr descriptor array:

```cpp
class Motor : public core::Module<Motor, Event> {
 public:
  static constexpr const char* name() { return "motor"; }

  static constexpr auto commands() {
    return std::array{
        DAVEOS_COMMAND(Motor, SetSpeed, "speed", "Set motor speed",
                       core::arg("speed").range(0u, 100u))};
  }

  core::Status SetSpeed(std::uint32_t speed) {
    I_("requested speed: %" PRIu32, speed);
    return core::Status::ok;
  }
};
```

`DAVEOS_COMMAND` captures `SetSpeed` as both the member-function pointer and its
logging name. Logs from this handler identify `motor.SetSpeed`. Task and command
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
remain literal. Typed handlers receive converted values after argument-count,
type, and inclusive-range validation. Declare one `core::arg("name")` per
parameter; add `.min(value)`, `.max(value)`, or `.range(low, high)` for numeric
constraints. Trailing `std::optional<T>` arguments become `std::nullopt` when
omitted. Metadata mismatches fail compilation. The adapter supports integral
types, `float`, `double`, `bool`, enum choices, and borrowed `std::string_view`, plus trailing
optional forms. It consumes whole tokens and rejects numeric overflow, floating
underflow, NaN, and infinity. Integers accept decimal and explicit `0x`/`0b`
prefixes; leading zeros stay decimal. Floats accept decimal/scientific notation.

Strict booleans accept exactly `true` and `false`. `.friendly()` accepts
case-insensitive `true/false`, `1/0`, `on/off`, `yes/no`, `enable/disable`,
`high/low`, and `set/clear`; unknown values are rejected.

Enums declared with `DAVEOS_ENUM` automatically provide choices for
`core::arg("mode")`, including `std::optional<Enum>` parameters. To customize
labels or select a subset, declare a static constexpr `std::array` of
`core::EnumChoice{"label", Enum::value}` and use `.choices<table>()` on the
argument. Actual enum values are preserved, including sparse/negative values.
Tables must be nonempty and names must be nonempty and unique ignoring case;
table/handler enum mismatches are compile errors. Distinct labels may alias the
same value. Matching uses the routing lazy matcher: exact matches win, otherwise
one unique case-insensitive prefix is accepted. Unknown choices return
`not_found`, ambiguous ones return `ambiguous_match`, and neither invokes the
handler. Quoted empty strings do not count as omitted optional arguments.
Help shows the choices and errors identify the failing argument. See the
[typed command examples](02-logging-and-commands.md).

Handlers needing custom syntax can instead receive `CommandArguments`, a
`std::span<const std::string_view>` valid only until they return, and validate
their own arguments. Register raw handlers without argument descriptors.
Typed text views have the same borrowed lifetime. Nested calls on the same dispatcher return `busy` and preserve
active views. Inputs need no terminating NUL; embedded NUL bytes are rejected.

Defaults are 256 input bytes and 8 tokens, counting the prefix and command.
To customize, use `CommandDispatcher<Event, decltype(modules), 512, 24>`.
Compile-time declarations allow at most six parameters, independently of the
dispatcher token capacity. A shared static table holds compact descriptors and
metadata only for actual parameters; help and dispatch reuse it. Numeric bounds
and choice policies share a variant, while type labels remain shared string pointers.
Range bounds are supported for integers up to 32 bits and `float`; unbounded
64-bit integer and `double` parsing remains available. These restrictions also
apply to optional parameters. The six-parameter limit does not restrict raw
handlers, which use the dispatcher token capacity. Overflows reject the complete line without invoking a handler. Specific results
are `parse_error`, `ambiguous_match`, `line_too_long`, and `too_many_arguments`;
unknown names return `not_found`. Handler results propagate unchanged. Typed
argument failures emit one specific diagnostic with the status, without a second
generic command-error line.

`help` lists the complete tree, short descriptions, and typed parameter
names/types (`<required>` or `[optional]`). `motor` or `motor help`
lists that module's commands; extra arguments to help are errors. All help and
errors use ordinary best-effort buffered logging under `core.command`. Output
buffer overflow and filtering apply just as for statistics tables; increase the
logger's capacity if the default cannot accommodate a full tree.

Build and run the host console example with:

```sh
meson compile -C build/host console-host
./build/host/examples/console/console-host
# Try: help, console echo "Hello World", console exit
```

The hello, system, and host console examples keep their passive modules at file
scope; the console input queue also has static storage. Their platform, logger,
and scheduler remain local to `main()`: constructing the host platform starts
its timer thread, and local lifetimes keep startup and shutdown explicit.
The console reader thread joins before scheduler/platform teardown.

Its input thread assembles lines into a bounded queue; a scheduled task dispatches
them. `console exit` requests shutdown and log flushing. EOF only ends input
collection and does not stop the scheduler. UART transport integration is left
to the application and is not part of this example.

## STM32H755 console (M7) and sleeping M4

The CubeMX project in `platform/stm32/nucleo/h755/` builds two hard-float images,
using the pinned STM32CubeH7 **v1.13.0** HAL and CMSIS without a board BSP:

```sh
git submodule update --init platform/stm32h7/STM32CubeH7
git -C platform/stm32h7/STM32CubeH7 submodule update --init --recursive \
  Drivers/CMSIS/Device/ST/STM32H7xx Drivers/STM32H7xx_HAL_Driver
git submodule update --init platform/stm32/STM32_USB_Device_Library
export PATH="$PWD/tools/external/arm-gnu-toolchain-15.2.rel1-x86_64-arm-none-eabi/bin:$PATH"
meson setup build/h755 --cross-file meson/stm32.ini -Dboard=h755 -Dexamples=true
meson compile -C build/h755
```

Outputs under `build/h755/examples/stm32_console/`:

| Core | Image | Flash base |
| --- | --- | --- |
| M7 | `stm32-console.elf` | `0x08000000` |
| M4 | `boards/h755/CM4/stm32h755-sleep-m4.elf` | `0x08100000` |

Each image also has `.hex`, `.bin`, and `.map` outputs in its directory. Program
both images and use the matching flash boot addresses with both cores enabled.
The M7 waits for the M4 to enter Stop during CubeMX's HSEM boot handshake, then
releases it after clock setup. The M4 finishes HAL initialization, disables its
SysTick, and remains in shallow WFI sleep. See ST's
[dual-core architecture note](https://www.st.com/resource/en/application_note/an5557-stm32h745755-and-stm32h747757-lines-dualcore-architecture-stmicroelectronics.pdf)
for the separate core power domains. DaveOS runs only on M7; its critical sections
do not synchronize shared state with M4.

Connect USART3 **PD8 TX / PD9 RX**, **1,000,000 baud, 8N1**, no flow control.
Disable local echo in the terminal: the console echoes input itself. CR, LF, and CRLF terminate input:

```text
help
board led 1 on
board led 2 toggle
board led 3 off
board button
board stats
board timer 1000000
board reset
```

`board timer <microseconds>` starts a one-shot DaveOS timer with a positive
unsigned decimal delay. Its interrupt-time callback queues a `Timer fired` log;
normal idle-time logging delivers it to the console outputs. Issuing the command
again replaces the pending timer. Invalid, zero, or overflowing arguments are
rejected. This command is shared by the H755 and H563 board consoles.

`board version` prints the application major/minor/build, Git commit and dirty
flag. Local build numbers appear as `local`; see [version identity checks](testing.md#version-identity-checks).

`board reset` takes no arguments and immediately resets the MCU (both cores),
discarding pending logs/output. The board module calls `platform.reset()` directly;
the scheduler does not manage reset. Host/fake platforms return `Status::unsupported`.

LEDs are LD1/PB0, LD2/PE1, and LD3/PB14; BTN1 is PC13 and reports its raw level.
Both H563 and H755 use the shared UART implementation: interrupts collect up to
16 complete lines; a 1 ms task dispatches one per invocation. The hardware FIFO absorbs short interrupt-masked intervals at
1 Mb/s. Each command may contain up to 256 bytes before its terminator. Queue
overflow drops whole new lines and records a warning; sustained input still
needs sender pacing. FIFO setup runs during module stage1, after CubeMX setup,
so regeneration cannot silently disable it. H563 hardware validation passed
580 unpaced commands, including repeated 16-line, 4,112-byte bursts and
overlength rejection followed by a valid command. H755 now has current
UART burst and UART/USB/TCP hardware coverage after the static-storage,
FIFO/queue, Meson board/component selection, convenience-API, and
Application-composition changes. H563 hardware smoke tests also passed after the Application-composition
migration: UART/USB/TCP commands, timers, statistics, LED command acknowledgements,
button reads, large-packet ping, TCP reconnect, and software-reset recovery.
Overlength lines are rejected, full queues drop entire lines, and UART errors
discard input through the next terminator. Dropped input is reported via logging.
Log calls queue records immediately with timestamps, severity, module, and
handler names. Scheduler idle time delivers them to subscribers, which append
CRLF. With `-Dlogging=false`, commands still execute but help and log output are silent. There is no exit command on embedded.

USART3 output (logs and echo) uses DMA1 Stream 0, memory-to-peripheral, with
normal byte transfers and the USART3 TX request. DMA completion enables the
USART3 transmission-complete interrupt, which releases the transmitted buffer
and starts pending output. Two 8 KiB ping-pong buffers live in DMA-accessible
AXI SRAM (`.dma_tx`); the linker section must be retained after CubeMX regeneration.
The driver cleans transmitted cache lines if D-cache is enabled. DMA and AXI SRAM
clocks remain enabled during shallow sleep. RX still uses one-byte interrupts.
Output never waits for space: a complete display/log frame (up to 768 bytes) is
dropped if it cannot fit. Transfer errors discard queued output with uncertain
progress. `board stats` reports bytes sent, transfers, dropped frames, and errors.
Both boards leave HAL's half-transfer interrupt enabled. Do not disable it by
rewriting the DMA control register after starting a transfer: hardware can clear
`EN` between the read and write, and the stale write can restart an exhausted
H563 GPDMA transfer with a user-setting error. The HAL half-transfer callback is
otherwise a no-op.

The [pytest HIL suite](testing.md) includes UART burst/max-length/overflow
recovery and transmit-counter checks. Each selected test begins with factory
programming; its transcripts and counters are saved under `build/hil/`. Keep
other UART readers and debuggers detached. The suite also checks USB and TCP;
physical LED/button validation remains separate.

The H755 also exposes a USB CDC ACM console on **CN13 (Micro-AB)**. Connect a
USB data cable there and keep ST-LINK connected for power/debugging. On Linux,
select `/dev/serial/by-id/usb-DaveOS_DaveOS_H755_console_*-if00` (typically a
second `/dev/ttyACM*`). Open it with a serial terminal that asserts DTR; its baud
setting is ignored. Disable local echo. Commands and log records are shared
with USART3, but each transport keeps its own partial-line buffer and echo.
Avoid opening multiple readers on the same port: they compete for received bytes.

UART and USB are independent modules (`uart` and `usb`). Each registers its own
logger subscriber and registers its command source with the shared dispatcher;
each enabled source polls its line buffer in its own scheduled task. The `board`
module provides hardware commands and does not forward transport input/output.

Select transports independently (both default to enabled on H563 and H755):

| Configuration | Meson options |
| --- | --- |
| UART and USB | `-Duart_console=true -Dusb_console=true` |
| UART only | `-Duart_console=true -Dusb_console=false` |
| USB only | `-Duart_console=false -Dusb_console=true` |
| Neither | `-Duart_console=false -Dusb_console=false` |

Application wiring registers modules, subscribers, and sources explicitly.
The shared example generates these lists for the selected components; the
corresponding manual setup is:

```cpp
auto modules = core::ModuleList{&board, &uart, &usb};
auto subscribers = core::SubscriberList{uart.subscriber(), usb.subscriber()};
auto logger = core::make_logger(platform, subscribers);
auto sources = core::CommandSourceList{uart.command_source(), usb.command_source()};
auto application = core::make_application<Event>(platform, modules, logger, sources);
// In the entry point, after all borrowed objects have been constructed:
const auto status = application.run();
```

Include `core/schedule/application.hpp`. Application owns the scheduler and
optional dispatcher and binds sources after successful stage2 completion, before
dispatch. No wiring module or reserved module name is needed. Logging and sources
can each be omitted independently. Use `application.scheduler()` for scheduling
and statistics, and Application for init/run. See the
[application guide](application-guide.md) for periodic task declarations
and the smaller standalone starter. Explicit scheduler/dispatcher wiring remains
available for custom composition.

`CommandSource` submits complete lines through `dispatch()`; the dispatcher owns
the only tokenizer. An unregistered source returns `Status::not_running`.
Registration borrows the dispatcher, which must outlive all submissions. Sources
submit from scheduled callbacks, never interrupts. An empty `CommandSourceList{}`
is valid; the existing direct `dispatcher.dispatch(line)` API remains available.

For example, `meson configure build/h755 -Duart_console=false`, then rebuild.
Disabled transports have no console module, input polling, or logger subscription.

Meson selects `uart.cpp`, `usb/component.cpp` (plus USB middleware), and
`network.cpp`; `tcp_console.hpp` is included only when both networking and TCP
are enabled. The always-present board commands live in `board.h`. There are no
`DAVEOS_UART_CONSOLE`, `DAVEOS_USB_CDC`, `DAVEOS_NETWORKING`, or
`DAVEOS_TCP_CONSOLE` preprocessor branches in the application.

`examples/stm32_console/meson.build` generates
`build/<configuration>/examples/stm32_console/composition.hpp` from
`composition.hpp.in`. This small header selects component members and creates a
`stm32::Console` group. The library group supplies module/subscriber/source lists,
statistics routing, and reverse-order console shutdown. It owns no transports. `appmain.cpp` constructs the resulting `Components`
aggregate; every constructor remains passive. Module initialization still runs
all stage1 callbacks before any stage2 callback, and Application binds command
sources after successful completion of stage2. Shutdown stops TCP before its network service. To add a transport, add
its ordinary source/header and its selection/registration in this Meson file;
do not edit the generated header. The `logging` option remains independent.
USB middleware is omitted when USB is disabled. CubeMX's existing USART3/DMA
peripheral initialization remains, but no UART console RX/TX is started when UART
is disabled. The same options apply to H563 using `build/arm`.
These options are independent of `-Dlogging=false`.

USB output reuses the bounded ping-pong buffer helper with two 4 KiB buffers.
The USB peripheral moves data through its FIFO in interrupts, without DMA or
heap allocation (H563 uses packet memory rather than a FIFO). `board stats`
includes USB transfer/drop counters. Output while unconfigured or DTR-low is discarded; backpressure drops whole new frames when
buffers fill. DTR deassertion, bus reset, and disconnect clear unfinished input
and queued USB output. UART remains available independently.

`platform/stm32/console/usb/` owns the shared console, CDC glue, and descriptors.
Both boards use ST's USB Device Library **v2.11.3**, pinned as a shared submodule
under `platform/stm32/STM32_USB_Device_Library` rather than depending on CubeH7.
Each example's `usb/` directory supplies its controller-specific setup.
H755 configures USB2 OTG FS, PA9 VBUS sensing,
PA11/PA12 data pins, IRQ priority 6, and HSI48 with USB2 SOF synchronization via
CRS. These pins must remain reserved. This setup is application-owned rather
than CubeMX-generated: do not generate a second USB stack or duplicate IRQ/
callbacks. Meson enables the HAL PCD component, so CubeMX regeneration does not
need to enable it in the generated HAL configuration. The demo uses ST's
example VID/PID `0483:5740` and a per-chip serial number.

The M7 uses the shared H5/H7 TIM2 adapter at 1 MHz and shallow sleep. TIM2 belongs
exclusively to DaveOS. The example uses the internal 64 MHz HSI RC oscillator, with PLL M=4/N=50/P=2
for a nominal 400 MHz M7 and 50 MHz TIM2 kernel clock. UART baud and timer accuracy
follow HSI accuracy; no external clock or solder-bridge changes are needed. Direct-SMPS
power configuration is retained. The M7 stack uses all remaining DTCM, with a 16 KiB minimum-headroom check. M7 logging uses full newlib from the toolchain: newlib-nano misread `%llu`
arguments and caused a hardware-confirmed HardFault in the log subscriber. The
M4 retains newlib-nano. All images reject heap growth. Built-in statistics avoid
floating-point formatting; see the [allocation audit](memory.md) for the
remaining libc paths and application-format limitations.
Core-specific Meson flags select M7 double-precision and M4 single-precision FPUs,
and explicitly locate each core's vector table in its own flash bank.

Keep the `.ioc`, `Common/`, both `Core/` trees, and flash linker scripts when
regenerating. CubeMX CMake/IDE files, copied `Drivers/`, and SRAM linker scripts
are ignored. Preserve the `USER CODE` hooks and the M7 stack setting. These images
have been programmed and verified through OpenOCD/GDB. Initial hardware checks
confirmed dual-core startup, M4 sleep, M7 scheduler idle, and approximate TIM2
rate; remaining validation is tracked in [TODO.md](../TODO.md).

## Fake time and host interrupts

The fake platform defaults to automatic advancement. When idle it advances to the
next task or timer deadline. `Platform::Advancement::manual` in
`daveos::platform::fake` lets a test advance time with `advance(microseconds)` or inject an interrupt explicitly. Due timer callbacks
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

GitHub Actions runs host/fake tests, a host build with logging disabled, sanitizers,
format/lint checks, and the ARM core, H563 console, and H755 M7/M4 firmware builds.
It also tests the separate starter as a Meson consumer, invalid API uses at compile
time, board switching, and independent STM32 transport/logging configurations. Allocation tests
instrument C++ `new` during representative core operations in ordinary and ASan builds (TSan owns its own allocator interceptors);
they do not certify allocator behavior inside every platform libc
formatting implementation. ARM firmware must validate its chosen libc as well.

Remaining work is tracked in [TODO.md](../TODO.md).

## Programming STM32 targets

With examples enabled, both STM32 build directories provide:

```sh
meson compile -C build/h755 flash-plan     # Build images and preview commands
meson compile -C build/h755 flash          # STM32CubeProgrammer: both H755 images
meson compile -C build/h755 flash-openocd  # OpenOCD: both H755 images
meson compile -C build/arm flash           # STM32CubeProgrammer: H563 image
```

Programming uses ST-LINK over SWD, connects under hardware reset, writes and
verifies the ELF images, then resets to run. Connect the probe's NRST signal.
For H755, both core images are built and programmed in one invocation before
resetting. Flash addresses come from the ELFs. These targets do not change boot
option bytes or request a whole-chip erase; configure both H755 cores' boot
addresses as described above. Programming targets are explicit actions and never
run as part of a normal build or test.

Tools are discovered under `~/install/stmicro/openocd/bin`,
`~/install/stmicro/STM32CubeProgrammer/bin` (also
`~/install/STM32CubeProgrammer/bin`), then on `PATH`. Override paths or select a
particular probe when needed:

```sh
meson configure build/h755 \
  -Dopenocd=/path/to/openocd \
  -Dcubeprogrammer=/path/to/STM32_Programmer_CLI \
  -Dprobe_serial=YOUR_STLINK_SERIAL
```

OpenOCD needs ST's H5/H7 target scripts and the `stlink-dap` interface; use the
installed ST distribution for H5 support. `flash-plan` never contacts hardware
and prints both commands even when programming tools are not installed.
Command construction is tested automatically. OpenOCD/GDB programming and image
verification have been exercised on both boards; those checks do not certify
every programmer backend or probe configuration.

### Debugging the H755

Start OpenOCD with both cores and hardware reset configured:

```sh
~/install/stmicro/openocd/bin/openocd \
  -f interface/stlink-dap.cfg -c 'transport select dapdirect_swd' \
  -c 'set DUAL_BANK 1; set DUAL_CORE 1' -f target/stm32h7x.cfg \
  -c 'reset_config srst_only srst_nogate connect_assert_srst'
arm-none-eabi-gdb build/h755/examples/stm32_console/stm32-console.elf
```

In GDB, use `target extended-remote localhost:3333`, `bt`, and `continue`.
The M4 is on port 3334 with its own ELF. Stop this OpenOCD instance before using
programming targets, which need exclusive access to the ST-LINK probe.

Initial hardware debugging found two independent faults: newlib-nano misparsed
`%llu` in logging, and the original 25 MHz HSE assumption produced approximately
320 kbaud from the board's default 8 MHz ST-LINK clock. M7 now uses full newlib and
the clock configuration uses the internal 64 MHz HSI oscillator at the application
owner’s preference. Selecting the fitted X2 crystal
instead requires the solder-bridge/capacitor configuration in UM2408 section 7.9.1,
plus corresponding 25 MHz crystal-mode firmware settings.

The logging-enabled hello, system, host console, and STM32 examples use
`[ddd:hh:mm:ss.mmm] L module.function: message`,
with the context left aligned in 22 columns. Names longer than that are
ellipsized for display; records retain their full names. Days expand past three
digits after 999 days. `daveos::core::LogPrefix<Width>` supplies the shared prefix
formatter; subscribers still choose the transport and line ending.

### Named enums

`core/enum/enum.h` generates scoped enums, `constexpr enum_name()` overloads,
and `enum_choices()` name/value tables
from a single list, with no allocation or separate string table to maintain:

```cpp
#define APP_STATES(X) X(idle) X(running) X(failed, 10)
DAVEOS_ENUM(State, std::uint32_t, APP_STATES)
#undef APP_STATES
// enum_name(State::failed) returns "failed" (const char*).
```

Declare these at namespace scope. Lookup works through ADL; explicit sparse or
negative values are supported, duplicate-value aliases are not. Values without
a named enumerator return `"unknown"`. `daveos::core::Status` uses this mechanism,
and command diagnostics print status names instead of numeric values.

The shared H563/H755 UART console echoes the pending line from its scheduled
input task. Newly appended characters are written once, rather than redrawing
the whole line at every poll; edits and intervening logs still redraw as needed.
Return clears that line with an ANSI erase-line sequence, then logs `> command` before
dispatch. Backspace/Delete remove the last character. Incoming logs temporarily
clear and redraw unfinished input. This is a single-line editor: use an ANSI
terminal and keep input within the terminal width. Echo remains available without
logging; the submitted-command record follows normal logging/filtering rules.

Integer logging uses fixed-width types and the `PRI*` macros from `<inttypes.h>`.
Embedded output avoids 64-bit integer printf conversions: `LogUnsigned` renders
values through `PRIu32`, showing `4294967295+` above that limit. Statistics
snapshots retain their full 64-bit values. Timestamp fields fit in 32 bits.
If 64-bit hex output is needed, format its upper and lower 32-bit halves
separately (padding the lower half to eight hex digits).

## Optional Ethernet networking

`-Dnetworking=true` adds a separate lwIP-based network service and the `net`
module to either board selection of the STM32 console. It is off by default
and independent of logging,
UART, and USB. No networking methods are added to the scheduler/platform API.
Networking supports IPv4 ARP, ping, DHCP/static addressing, and a single-client
TCP console. IPv6, DNS, TLS, fragmentation/reassembly, and a general connection
API are not implemented yet. UDP is enabled for DHCP, without an application UDP API.

Initialize the additional pinned submodules and build:

```sh
git submodule update --init net/lwip platform/stm32/lan8742
meson setup build/net-h755 --cross-file meson/stm32.ini -Dboard=h755 -Dexamples=true -Dnetworking=true
meson compile -C build/net-h755
# H563: use build/net-h563 and omit -Dboard=h755 (or set -Dboard=h563).
```

Keep the regular HAL/CMSIS/USB dependency setup from the board sections above.
Use the chosen build directory's `flash`/`flash-openocd` target for programming.
Connect RJ45 CN14 to a LAN with DHCP and issue `net status` through UART or USB.
Link/address changes are also logged. If logging is disabled, the network still
works but status output is silent. H755 needs Ethernet jumpers JP6 and JP7 fitted;
H563 needs JP6. Retain stock RMII solder bridges. The PHY supplies the 50 MHz RMII
reference, independently of the internal HSI CPU clock.

The demo's configuration is passed to `net::Service` in
`platform/stm32/console/network.hpp`, through a configuration factory invoked
in stage1. It defaults to DHCP and a locally administered MAC derived from the
MCU UID. For static addressing, supply a factory that sets `dhcp = false` and
the `address`, `netmask`, and `gateway` arrays; the
configuration defaults for static mode are 192.168.50.2/24 with no gateway.
Override `mac` if the deployment assigns MAC addresses; the UID hash is a demo
convention, not an assigned globally unique address. Hardware-init faults are
reported without preventing other modules from running; reset retries hardware
initialization. Cable/DHCP recovery is automatic.

`net/service.h` is independent of DaveOS and lwIP headers. The application owns
its borrowed driver and clock and calls `init()`, `poll()`, and `snapshot()`.
One service may be active per process because NO_SYS lwIP has global state.
Initialization is single-use; destruction stops the interface. All service
access is serialized in one caller context, never from interrupts.
`net/module.hpp` provides the thin DaveOS adapter and `net status` command.
The module polls every 1 ms, consumes at most four frames per invocation, runs
lwIP timeouts even without traffic, and samples link status every 250 ms.

lwIP 2.2.1 and LAN8742 are pinned submodules; the MAC drivers come from the
existing H5/H7 HAL. lwIP uses fixed static pools, including sixteen 1536-byte
RX pbufs and fixed 256/768/1600-byte allocation buckets, never libc allocation.
The driver uses eight RX descriptors, sixteen RX buffers for replacement, four
TX descriptors, and four TX buffers. Frames are copied between these DMA
buffers and lwIP; TX ownership lasts through completion. RX processing,
backpressure drops, reconnect teardown, and malformed packets cannot borrow
application storage. Interrupt handlers never call lwIP.

The H755 linker reserves 0x24070000–0x2407ffff for `.eth_dma`, avoiding DTCM,
UART DMA storage, and M4 memory. MPU region 7 marks it noncacheable, including
when D-cache is enabled. H563 uses ordinary SRAM with its current cache setup.
Keep these linker sections, H755's eight RX descriptors in the HAL config, and
board Ethernet setup after CubeMX regeneration. Pin/clock setup remains owned
by the example, not a board BSP. H755 RMII TXD1 is PB13; H563's is PB15.

Host tests use the real lwIP stack with fake Ethernet frames and a fake clock:

```sh
meson setup build/net --native-file meson/clang.ini -Dnetworking=true
meson test -C build/net --print-errorlogs
```

They cover ARP, ICMP echo, DHCP acquisition/retry and clock wrap, link changes,
RX budget, exhausted packet pools, bad frames, and unrelated-module operation
after hardware initialization failure. Allocator interception checks that stack
initialization, packet processing, and shutdown do not use the runtime heap.

On NUCLEO-H755ZI-Q, hardware checks passed for 100 Mbit full-duplex link,
DHCP, static IPv4, ping (including 1,400-byte payloads), cable reconnection,
and USB console responsiveness. Static addressing was tested by overriding
the configuration in RAM before service initialization, using the board's
current DHCP lease address; a reset restored the default DHCP configuration.
The RX adapter treats a null chain head as a new packet: ST's HAL retains the
previous tail after delivery, so testing that tail would leak receive buffers.
H563 Ethernet has passed hardware checks for DHCP, 100 Mbit full-duplex link,
1,400-byte ping payloads, TCP commands, rejection of a second client, and TCP
reconnection, including DHCP recovery after physically unplugging Ethernet.
Static IPv4 remains to validate on H563 hardware.


With networking enabled, both STM32 demos also register an independent TCP log
subscriber and command source on port **1000**. Connect using the address from
`net status`, for example:

```sh
nc 192.168.1.147 1000
# Then type help, net status, board timer 100000, etc.
```

Only one active client is accepted; additional clients are reset. Closing the
connection discards partial input and queued output. The next connection starts
fresh. Logs are shared across all subscribers, including commands entered on
UART or USB. Input uses the common line collector and dispatcher; the local
terminal supplies echo. This is plain TCP, not Telnet or TLS, and has no
authentication. Peer half-close also ends the session, so keep the connection
open while waiting for command output.

`-Dtcp_console=false` removes this transport while retaining networking. UART,
USB, and logging remain independently selectable. To change the port, pass it
to the application's `TcpConsole` constructor. `net/tcp_server.h` exposes the
DaveOS-independent, nonblocking `TcpServer` used by this adapter. Its service
must outlive it; stop the server before stopping the service. Call both from the
same serialized context. It uses fixed 4 KiB input and 8 KiB output buffers;
TCP receive-window flow control handles full input buffers, and slow-client
output overflow drops whole records (`dropped_output()` counts these drops).
No logs are saved for disconnected clients. lwIP has fixed pools for one
listener, four TCP PCBs (including handshakes), and 24 segments.

Earlier H755 hardware checks passed for TCP `help`, `net status`, asynchronous
timer logs, rejection of a second client, and reconnect after an unfinished command.
Packet tests also cover a full receive window, complete-record output overflow,
peer FIN/reset, and link-loss recovery. H563 has build coverage, including TCP
without UART/USB/logging and networking without the TCP console. Its UART,
USB, Ethernet, and TCP console also pass initial hardware bring-up checks.


Shared console helpers live in `console/` under `daveos::console`, with an
explicit `console_dep` Meson dependency for transports and their tests.
`Input` collects lines; `LineDisplay` handles terminal presentation through a
borrowed callback context; `BufferedOutput` manages bounded asynchronous output
for UART DMA and USB. `Module<Derived, Event>` supplies the common 1 ms task,
dropped-input reporting, command logging/dispatch, and source/subscriber
registration. Transports retain their own session, echo, and transfer behavior.

The application owns the USB transport as well as the UART and TCP objects.
UART/USB C callbacks keep only routing pointers, detached after hardware is
quiesced. USB rejects concurrent activation and repeated initialization; failed
initialization releases the route. lwIP's process-wide stack remains a library
constraint, not a shared singleton abstraction for the transports.

TCP input is a ring buffer. The console inspects a contiguous span and consumes
only through the first completed line, with a 256-byte budget per invocation.
Remaining commands stay in the ring for later invocations; a partial line stays
in `Input`. CRLF works across chunk and ring boundaries. Receive-window credit
is returned only for consumed bytes, and consumption no longer shifts the
remaining buffer. Tests cover command bursts, partial tails, ring wrap, and USB
callback ownership across failed initialization, stop, and a new instance.

Before the later board/composition and convenience-API changes, H755 hardware
checks passed for USB commands, multiple TCP commands in one burst, completion of a partial command in a later
packet, and delivery of USB-originated command logs to the TCP subscriber.

After extracting the reusable console and Nucleo support, H563 checks passed
over UART, USB, and TCP, including timers, Ethernet ping, reconnect, and software
reset recovery. The standalone H563 starter also passed periodic-worker and UART
command/reset checks. Both H563 and H755 starters build independently and their
programming plans select the correct images, including H755's sleeping M4.
The current H755 console and reliability paths have also been hardware-tested;
its standalone starter still has build/programming-plan coverage only. The
[factory-image guide](03-hardware-console.md#h755-factory-boot-image) describes
H755 A/B builds, which include the fixed M4 in one factory HEX file.


For the full H563 console matching the H755 configuration:

```sh
meson setup build/net-h563 --cross-file meson/stm32.ini -Dexamples=true -Dnetworking=true
meson compile -C build/net-h563
meson compile -C build/net-h563 flash-openocd
```

Keep ST-LINK connected, connect CN13 USB-C for the independent USB console,
and CN14 to a DHCP LAN. `net status` reports the address for `nc <address> 1000`.
Use `target/stm32h5x.cfg` for H563. Both boards may remain connected: select each
ST-LINK serial explicitly and give their OpenOCD servers separate ports, as
shown in [Testing](testing.md#h755-reliability-and-ab-hil). If GDB attachment cannot halt the old firmware,
issue `reset halt` through OpenOCD before attaching.

## Payload events

Declare an application variant and register only the payloads each module handles:

```cpp
#include <cinttypes>
#include "core/schedule/module.hpp"

namespace core = daveos::core;

struct ButtonPressed { std::uint8_t button = 0; };
struct TemperatureChanged { float celsius = 0; };
using Event = std::variant<ButtonPressed, TemperatureChanged>;

struct Controller : core::Module<Controller, Event> {
  static constexpr const char* name() { return "controller"; }

  static constexpr auto events() {
    return std::tuple{DAVEOS_EVENT(Controller, OnButton)};
  }

  void OnButton(const ButtonPressed& event) {
    I_("button %" PRIu32, static_cast<std::uint32_t>(event.button));
  }
};

// From a module callback; excludes this module from the broadcast:
// scheduler().post(ButtonPressed{1}, this);
```

`TemperatureChanged` is ignored by this controller. To handle the full variant,
override `void on_event(const Event&)` and call `std::visit` instead of registering
`events()`. These two styles are mutually exclusive. Named handlers log under
`controller.OnButton`; a custom visitor logs under `controller.on_event`.

Posting copies the payload into the fixed-capacity event queue, including from
interrupts. Receivers borrow it only for the duration of their callback. Payloads
must be unique variant alternatives, trivially copyable and nonthrowing default/
copy constructible and copy assignable. The variant must be trivially copyable.
Use small owned payloads; views and pointers require separately managed lifetimes.
Queue storage scales with the largest alternative. Sender exclusion, unspecified
recipient ordering, and overflow reporting are unchanged. Use
`core::NoEvent` (an alias for `std::variant<std::monostate>`) for an application
with no events. `Module<Worker>`, `make_scheduler(...)`, and
`make_application(...)` default to that type.

### Named application capacities

```cpp
auto app = core::make_application<Event, core::Capacities{.events = 64}>(
    platform, modules, logger, sources);
```

The fields are `events`, `timers`, `line`, and `arguments`, defaulting to
32, 16, 256, and 8. `yield_depth` defaults to 4 active task callbacks
(including the outer task); values 0 or 1 disable nested task dispatch.
Command capacities apply only with command sources.
For event-free applications omit the event argument:
`make_application<core::Capacities{.events = 64}>(platform, modules)`.
Both capacity-first (optionally followed by Event) and Event-first forms are
available. Logger overloads enforce the complete
`LoggerFor<L, P>` contract. Low-level `make_scheduler` numeric capacities are unchanged.

### Command storage measurements

With the ARM toolchain and H563 debug (`-O0`) configuration, enabling logging,
UART, USB, networking, and TCP, commit `88f03b5` reduced ELF text+data from
332,292 to 325,276 bytes and BSS from 162,008 to 161,944 bytes. These are build
measurements, not hardware validation or release-optimization measurements.

Argument metadata is 28 bytes on ARM (previously 40); a compact command descriptor
is 32 bytes. The six-parameter declaration builder is 192 bytes, but the dispatcher
stores only the compact descriptors and actual argument entries. Help and dispatch
share those entries. Type labels remain shared string pointers: replacing them
with tags made individual records smaller but increased this firmware's code size.

### Migrating commands with many raw arguments

The default changed from 16 tokens to 8, **including raw `CommandArguments`
handlers**. Module and command names consume two tokens, so ten raw arguments
require at least `.arguments = 12` in the application's `Capacities`, or a token
capacity of 12 on a directly constructed `CommandDispatcher`. Otherwise dispatch
returns `too_many_arguments` without invoking the handler. Typed handlers remain
limited to six parameters even with a larger token buffer.

Bounds on 64-bit integer or `double` parameters are compile errors. Use a 32-bit
integer or `float`, or parse the wide value without bounds metadata and check its
range inside the handler.

## SPI/I2C HAL

`daveos-hal` exposes callback-based controller/master transactions, borrowed
aliased device handles, and a polling completion helper. SPI uses eight-bit
words and GPIO chip selects; I2C uses unshifted seven-bit addresses.
See [SPI/I2C API and wiring](spi-i2c.md) and the
[platform contract](spec/platforms.md#spi-and-i2c-hal).

### Cooperative task yielding

`scheduler().yield()` dispatches at most one other due task and returns; it
never sleeps, advances fake time, dispatches events, or drains logs. Calls
outside this scheduler's task call chain return and count `invalid_context`.
Active task callbacks cannot reenter, even if rescheduled. Nesting is bounded
by `Capacities::yield_depth` (default 4). Full contract and examples:
[Task yielding](yield.md).

Task durations retain elapsed-time semantics. `total_self_duration` excludes
nested task callback durations; `total_nested_duration` records that excluded
time. Self time still includes ISR time and waiting. The statistics table
includes both totals and the `invalid_yields`/`yield_depth_errors` counters.
