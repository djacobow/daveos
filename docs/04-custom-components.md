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
that object and adds the module interface. Its transport contract is:

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

Use `make_scheduler`, an application-owned Logger, and CommandDispatcher directly
when Application's ownership/lifecycle is not a fit. Bind command sources during
initialization, before dispatch. The low-level CommandBinding helper is available
for stage2 wiring. Do not create an Application just to bypass its lifecycle via
its scheduler accessor.

Use the [API guide](application-guide.md) for precise callback, time, and failure
contracts, and [PROJECT.md](../PROJECT.md) for scheduler semantics. Keep drivers
and services separate from DaveOS where possible; the independent lwIP service
and its small scheduling module are examples of that separation.
