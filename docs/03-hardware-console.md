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
`board stats`, `board timer`, `board version`, and `board reset`. Board commands use the configured
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

## H563 factory boot image

The H563 example can boot through the small CRC-verifying bootloader. With the
ARM toolchain on `PATH`, build a factory image with:

```sh
meson setup build/boot-h563 --cross-file meson/stm32.ini -Dboard=h563 -Dexamples=true -Dnetworking=true -Dbootloader=true
meson compile -C build/boot-h563
meson compile -C build/boot-h563 flash-plan
meson compile -C build/boot-h563 flash
```

`examples/stm32_console/factory.hex` under that build directory contains the
bootloader, two copies of initial metadata, and confirmed application A linked
at `0x0800A000`. The bootloader reservation is fixed at 32 KiB; each application
slot has 984 KiB. The factory programming targets erase **all main flash** before
programming and verification, clearing any previous images and installation
history while preserving OTP. `flash-openocd` selects OpenOCD instead of
STM32CubeProgrammer; release any existing debug session before using either.

At 1 Mb/s on the ST-LINK UART, expect `Boot slot A (confirmed), CRC verified`
followed by the normal application messages. `board reset` returns through the
bootloader and repeats CRC verification. Bootloader-enabled builds also provide
`boot status` (executing slot and eligibility) and `boot confirm` (explicit,
durable, idempotent confirmation by the running application). The H563 health
module confirms automatically after five healthy seconds.
OTA must still be explicitly enabled as described below.

The same build links `stm32-console-b.elf` at `0x0810A000`, reusing the exact
objects compiled for A. It also builds `application.ota`: packaging must reproduce
the independent B binary byte-for-byte using block-local relocation records, or
the build fails. The B binary alone does not make the slot bootable: installation
must verify flash and commit pending metadata. Factory programming still clears
both slots and installs confirmed A; it is not a slot-B update command.

The [pytest HIL suite](testing.md) provisions a factory image before every
selected test, then checks UART/USB/TCP commands, reset recovery and networking.
Use its local board configuration instead of passing transport paths to separate
smoke scripts.

## TCP firmware updates

With `-Dbootloader=true -Dnetworking=true`, the example includes an independent
binary OTA listener on **TCP port 1001**. The text console remains on port 1000.
On any console, run `ota enable`, then upload from the host:

```sh
python3 tools/ota.py 192.168.1.207 build/boot-h563/examples/stm32_console/application.ota --reboot
```

Use the board's current DHCP address from `net status`. The running image must
be confirmed. The package works in either direction: the updater erases and
programs the inactive slot while normal application tasks continue, verifies
its CRC by reading flash, then commits it as a pending installation. The script
polls readiness and sends one checked chunk at a time. `--reboot` requests a
delayed application reset only after successful installation; omit it to keep
the current application running. This does not confirm the new image.

After the trial boots, inspect `boot status`; health-based confirmation takes
five seconds. `boot confirm` remains an explicit bring-up override.
A reset before confirmation rejects that trial and returns to the other valid,
confirmed image. `ota status` reports progress; `ota disable` closes the listener
and aborts unfinished work. Updates start disabled after every reset. Disconnecting
mid-upload discards progress: a new connection restarts from the beginning.
UART and USB remain text-only console transports.

The HIL OTA test checks CRC rejection, disconnect abort, bidirectional uploads,
concurrent TCP timers, reboot and automatic health-based confirmation. It starts
from a fresh factory image; see [Testing](testing.md).

Without `-Dbootloader=true`, the example retains its standalone linker layout
and normal programming behavior. H755 bootloader integration remains deferred.

Next: [custom components](04-custom-components.md).

The H563 demo starts IWDG before clock/peripheral initialization and feeds it
only after heartbeat and repeating-task progress checks pass. `health status`,
`health fault`, and `health clear` inspect health and retained diagnostics.
An early-warning interrupt saves the interrupted frame before the watchdog
resets the CPU. With a debugger attached it breaks first; resume past the
breakpoint to allow the reset. Masked interrupts can prevent capture, but do
not prevent the watchdog reset. H755 watchdog integration remains deferred.

H563 board support also links assembly HardFault, MemManage, BusFault and
UsageFault handlers, using the same reserved stack and retained record. Startup
enables the configurable fault exceptions. CubeMX's C fallback handlers are
weak through its preserved USER CODE block. Capture never logs from exception
context; startup reports the fault name, CFSR/HFSR, stack pointer and available
PC/LR/xPSR. Resume past the debugger breakpoint to reset.

The HIL fault tests independently inject all four CPU faults and verify retained
frames, status registers and recovery. They build and program matching firmware
before using GDB; see [Testing](testing.md).
