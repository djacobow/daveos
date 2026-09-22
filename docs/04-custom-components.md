# 4. Custom components

Keep the same vocabulary as the earlier steps. A module does cooperative work;
a platform supplies timing and synchronization; Application owns dispatch;
subscribers receive records and command sources submit lines.

## A new console transport

Use `daveos::console::Module<Derived, Event>` from `console/module.hpp` for a
transport with its own polling/output behavior. Supply `name()`, `poll_line(Line&)`,
`take_dropped()`, and `output(const LogRecord&)`. The base supplies an independent
subscriber/source and dispatches at most one complete command per invocation.
Incoming bytes still belong to your transport; `console::Input` and
`console::BufferedOutput` are reusable fixed-storage helpers.

For a serial transport object, `console::TransportModule<Event, Transport>` owns
that object and adds the module interface. `Transport::name()` is the default
module name; pass a second constructor argument to name an instance when one
transport type is used twice. Its transport contract is:

- Passive construction from a borrowed platform/context reference.
- `static name()` and `static statistics_label()` returning stable strings.
- `bool init()` and `void stop()` for hardware lifetime.
- `bool poll_line(Line&)`, `uint32_t take_dropped()`, and `void output(const LogRecord&)`.
- `TxCounters counters()` for the standard statistics report.

See [UART](../platform/stm32/console/uart.hpp) and
[USB](../platform/stm32/console/usb/usb.hpp). Their middleware/HAL callback routes
are pointers to application-owned transports because those C APIs lack a context
argument. They do not own application state. A second active instance of the same
physical peripheral is rejected; stop quiesces callbacks before detaching it.
The Nucleo UART adapter currently targets USART3 and one board-provided DMA buffer;
use your own transport adapter for another UART.

Any module exposing `subscriber()`, `command_source()`, and `stop()` can join
`stm32::Console`. An optional `log_statistics(scheduler)` is detected at compile
time. The group borrows objects and stops them in reverse registration order.
Keep network services outside the group and stop them afterward.

## A different board

The ready-made dependencies are `daveos-nucleo`, `daveos-stm32-console`,
`daveos-stm32-uart`, and (when enabled) `daveos-stm32-usb`. Enable `stm32_support`
when consuming them without DaveOS's examples. Nucleo support selects the board
at Meson setup and propagates CPU/ABI flags and linker settings to the application.
USB and networking remain explicit options with their own upstream submodules.

For custom hardware, keep your startup code, HAL configuration, linker script,
and `board_config.h` in your application. The Nucleo files under
`platform/stm32/nucleo/` document the current contract: platform type, timer clock,
LED/button functions, UART DMA start/storage/IRQ, and optional USB/Ethernet glue.
The console dependencies carry source files so they compile against your board
configuration. They do not smuggle in an example application or board BSP.

## Explicit composition

### File-backed flash for boot and OTA simulations

`update::Engine` and `boot::Control` accept an injected `boot::Flash` interface.
Use `platform::host::FileFlash` from `platform/host/flash.h` to exercise the same
code against a persistent host file. Link the `daveos-file-flash` and
`daveos-update` Meson dependencies (available in host and fake builds):

```cpp
namespace host = daveos::platform::host;
namespace boot = daveos::boot;
namespace update = daveos::update;

host::FileFlash flash;
auto status = flash.open("build/flash.bin", {0, 4096, 512},
                         host::FileFlash::OpenMode::create);
if (status != daveos::core::Status::ok) {
  return status;
}
boot::Layout layout{{1024, 3072}, {0, 512}, 1024, 512, 16, 1, 1};
update::Engine updater(flash.driver(), layout, 0);
```

Initialize the image and confirmed factory metadata through `boot::Journal`
before enabling uploads. `OpenMode::create` creates a new erased file and refuses
to overwrite an existing one. Omit that argument to reopen an existing image;
pass the same geometry each time. Bytes are stored at `address - base`, so a file
can also model the H563's physical addresses. No header is added to the raw image.

The backend enforces sector alignment, 16-byte programming, bounds, and NOR
one-to-zero programming rules. Each completed mutation is flushed with `fsync`.
Only one driver can own a file at a time; calls on that driver must be serialized.
The default clock is `steady_clock`; inject a context and microsecond clock
callback into the constructor for fake-time tests.

Host file operations execute synchronously in `poll()`. This models persistence
and flash rules, not real-time flash latency, STM32 ECC, or torn writes during
power loss. The separate memory flash tests inject torn writes and erases.
Closing before an operation executes drops that pending operation. The
[file-backed OTA test](../tests/reliability/file_flash.cpp) demonstrates upload,
close/reopen, trial boot, durable confirmation, and another simulated reboot.

### Direct scheduler composition

Use `make_scheduler`, an application-owned Logger, and CommandDispatcher directly
when Application's ownership/lifecycle is not a fit. Bind command sources during
initialization, before dispatch. The low-level CommandBinding helper is available
for stage2 wiring. Do not create an Application just to bypass its lifecycle via
its scheduler accessor.

Use the [API guide](application-guide.md) for precise callback, time, and failure
contracts, and [PROJECT.md](../PROJECT.md) for scheduler semantics. Keep drivers
and services separate from DaveOS where possible; the independent lwIP service
and its small scheduling module are examples of that separation.

### Structured state machines

[`core/state_machine/state_machine.hpp`](../core/state_machine/state_machine.hpp)
provides an allocation-free CRTP helper, independent of the scheduler. Use a
contiguous `enum class` starting at zero; supply its state count as the final
argument. Declare the enum and machine in the narrowest scope that their users
need (for example, nested in the owning service).

```cpp
namespace core = daveos::core;

enum class State { idle, running };

class Machine : public core::StateMachine<Machine, State, State::idle, 2> {
  friend class core::StateMachine<Machine, State, State::idle, 2>;

 public:
  void request_start() { start_ = true; }

 private:
  void Step(State cs, State& ns) {
    switch (cs) {
      case State::idle:
        if (start_) {
          ns = State::running;
        }
        break;
      case State::running:
        break;
    }
  }

  bool start_ = false;
};
```

Call `tick()` from a task or another serialized owner. It returns `Status::ok`
on a normal tick. The derived `Step` owns the transition switch, and the base
alone commits the selected next state. Optional `void OnEnter(State)`,
`void OnExit(State)`, and `void OnTick(State)` hooks perform state-specific actions;
use the state argument to select those actions. Neither hooks nor request methods
may change the current state. Hooks and `Step` must not throw.

Construction calls no hooks. On the first tick the initial state's entry hook
runs, then its dwell and cumulative tick counts increment, then `OnTick` and
`Step` run. On a transition, exit runs with the old state and its final dwell,
then the base changes state, resets dwell to zero, and calls entry. The new
state's tick happens on the next invocation. Staying in the same state does not
run exit/entry hooks.

- `state()` returns the current enum value.
- `dwell_count()` returns ticks spent in the current visit.
- `statistics(state)` returns cumulative `ticks` and `entries` for that state.
- `statistics()` returns a copy of the whole statistics array, indexed by enum
  value. Snapshots cannot mutate the machine. Unknown enum queries return zeros.

Counters are `std::uint64_t` and saturate at their maximum. They measure calls,
not elapsed time, and there is no implicit statistics reset. Recursive `tick()`
calls return `busy`. An out-of-range next state returns `invalid_argument` with
no transition hooks or state change; the attempted tick remains counted.
The helper is not thread-safe: serialize calls and snapshots, and synchronize
flags written by interrupts separately.

The [OTA engine](../update/engine.h) uses a private nested machine and exposes
these getters through its existing public interface. The journal, OTA writer,
package reader, protocol, watchdog controller, confirmation gate, host FileFlash,
and scheduler lifecycle also use the helper. Implementation-only enums stay
private; enums used by public observers remain nested and public. Network status
snapshots and persisted image eligibility remain data, not transition machines.

A machine can borrow context and inputs for a tick rather than store an owner
pointer: `machine.tick(owner, input, consumed)` calls
`Step(State cs, State& ns, Owner& owner, Input input, std::size_t& consumed)`.
Any optional hooks accept the same trailing arguments. Arguments are passed as
lvalues and never stored or moved from by the helper; callbacks must not retain
references to temporaries. The parsers use this to return consumed byte counts
without adding temporary fields. Constructors remain passive.

Scheduler lifecycle ticks occur at initialization, run, and shutdown boundaries
under the existing platform guard. Their counts measure lifecycle requests,
not scheduler loop iterations. Watchdog failure notification is an entry action,
so a latched failure notifies exactly once.

## Portable SPI/I2C drivers

Inject a `hal::spi::Device` or `hal::i2c::Device` into a peripheral driver. The
application owns controller configuration and alias registries; the driver owns
its transaction descriptors and buffers. See [SPI/I2C](spi-i2c.md) for callback
and polling-helper examples, STM32 IRQ wiring, and the read-only H563 SD fixture.

## Asynchronous drivers

Long operations are state machines advanced from a task, not blocking calls.
New drivers should use the shape `drivers/mcp3425.h`, `storage/sd/initializer.h`
and `storage/sd/session.h` share:

- `request()` accepts work or returns `busy`; it starts nothing inline.
- `tick()` advances the state machine from a task; interrupts only publish
  completions.
- `result()` (or `ready()` plus accessors) reports the outcome once it is final.
- `reset()`, where recovery is possible, is itself a request.

Existing HAL and flash interfaces keep their own shapes (`start()` with a
callback or `Completion`; `erase`/`program` plus `poll()`); do not wrap them just
to match.

Every public asynchronous API states its contract at the top of its header
under six labels, as the HAL, boot flash, update engine, SD and storage headers
do:

| Label | Answers |
| --- | --- |
| Admission | What is rejected, and does rejection change anything? Can completion happen inline? |
| Ownership | Which buffers and descriptors are borrowed, and until when? |
| Execution | Which context calls it, and which context completes it? |
| Deadline | What bounds it, and does expiry release buffers or only request cleanup? |
| Failure | What latches, and which explicit call clears it? |
| Lifetime | What must stay alive or be quiesced before destruction? |

Inject dependencies in one of three ways, chosen by what the boundary needs:

- A struct of function pointers with a `void*` context for runtime-swappable
  device boundaries (`storage::BlockDevice`, `boot::Flash`, `watchdog::Driver`,
  the SD session's hooks).
- A template parameter where a per-transfer path must stay static
  (`hal::Controller<Backend, Clock, Critical, N>`), or where a policy supplies
  static services (`watchdog::HealthModule<..., Hardware>`).
- CRTP only for the platform.

Report failure causes as a status, not a `bool`, and map them once at each
boundary: a HAL status becomes a `core::Status` in the SD session, which becomes
a FatFs result in the disk bridge. Nested `scheduler().yield()`, and
`core::wait_for_release()` built on it, are for wrapping synchronous libraries
such as FatFs, not for ordinary drivers; see [task yielding](yield.md).
