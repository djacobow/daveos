# 3. A hardware console

Use the [STM32 starter](../starters/stm32/README.md) for an independent application
on NUCLEO-H563ZI or NUCLEO-H755ZI-Q. It supplies `appmain()` and a periodic worker;
the `daveos-nucleo` Meson dependency supplies known startup, linker, clocks,
LED/button, and USART3 support. The H563 is the default board. H755 support also
builds the M4 image that completes boot synchronization and sleeps.

The application remains explicit:

```cpp
namespace stm32 = daveos::platform::stm32;
stm32::UartConsole<Event> uart{platform};
stm32::Console console{uart};
auto logger = core::make_logger(platform, console.subscribers());
auto app = core::make_application<Event>(
    platform, console.modules(board_module, worker), logger, console.sources());
```

`Console` borrows transports; it does not allocate, own hardware, or create a
singleton. It provides their module, subscriber, and source lists. With additional
transports, use `stm32::Console{uart, usb, tcp}`; with none, use `stm32::Console{}`.
UART and USB use `stm32::UartConsole<Event>` and `stm32::UsbConsole<Event>`.
TCP uses `daveos::console::TcpConsole<Event, Platform>` and borrows an independent
lwIP service. These headers/components have separate dependencies so unused
transports stay out of the build.

The library's `stm32::Board<Event>` provides `board led`, `board button`,
`board stats`, `board timer`, and `board reset`. Board commands use the configured
board's hardware functions; the scheduler itself knows nothing about LEDs or
console transports.

The starter uses USART3 at **1 Mb/s, 8N1**, through ST-LINK's virtual serial port.
The full [STM32 example](../examples/stm32_console/appmain.cpp) adds independently
selected USB and Ethernet/TCP support. Its Meson-generated header selects objects;
registration and shutdown behavior live in the library's Console group.

All long-lived application objects may have file scope. Generated `main()` calls
`appmain()` after peripheral setup. `appmain()` initializes the platform timer,
then calls `app.run()`. Transport setup waits for stage1. Stop consoles before
their network service or platform, so outstanding transfers are quiesced first.

See the [board/programming reference](reference.md) for cable connections,
submodule setup, build options, and flashing. See [TODO.md](../TODO.md) for the
precise hardware validation coverage; a successful cross-build is not a hardware test.

Next: [custom components](04-custom-components.md).
