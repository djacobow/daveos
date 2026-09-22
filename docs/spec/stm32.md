# STM32 modules and board integration

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

This document owns the hardware-specific integration. Portable algorithms and
modules retain their contracts in the [services specification](services.md).
The support matrix identifies which services have drivers for each family.

## Contents

- [Support and validation matrix](#support-and-validation-matrix)
- [Remaining platform validation](#remaining-platform-validation)
- [SPI/I2C validation fixtures](#spii2c-validation-fixtures)
- [Networking and application composition](#networking-and-application-composition)
- [Console library and device starter](#console-library-and-device-starter)
- [Flash layout and executable images](#flash-layout-and-executable-images)
- [Watchdog startup and application health](#watchdog-startup-and-application-health)
- [Fault-handler integration](#fault-handler-integration)
- [Implementation and validation gates](#implementation-and-validation-gates)
- [Test organization and board-specific HIL](#test-organization-and-board-specific-hil)
- [OTP integration](#otp-integration)

## SPI/I2C validation fixtures

The user has connected an SD card to the NUCLEO-H563ZI connector marked
"SPI A", with this wiring:

| Signal | MCU pin |
| --- | --- |
| SCK | PA5 |
| MISO | PG9 |
| MOSI | PB5 |
| CS | PD14 |

The CubeMX H563 device database confirms SPI1 with AF5 on these bus pins.
The optional `spi_sd_probe` module uses HSI/CKPER, starting at 250 kHz and
checking sector reads at 1 MHz. Five complete read-only inspections passed,
including CSD/CID, OCR, and 60 CRC-checked sector reads across both rates.
The connected 32 GB card has a primary FAT32 partition at LBA 8192, with
32 KiB clusters. No card sectors were written and the filesystem was not
mounted. Other SPI modes, higher speeds and filesystem contents remain
unqualified by that inspection. Optional `fatfs=true` adds
[read-only filesystem commands](../storage.md) after `sd probe`.
The subsequent filesystem HIL run mounted and listed the empty root,
checked errors and remounting, and retained healthy watchdog/fault status.
It made zero heap requests and used 2,936 bytes of the painted stack.
A later populated-card check passed 69 reads across six files, including
subdirectories, a long filename, sector/cluster boundaries and EOF behavior;
watchdog/fault checks stayed healthy. No card sectors were written. See the
[storage validation notes](../storage.md#validation) for scope and limits.
See [SPI/I2C](../spi-i2c.md).
The optional filesystem now supports explicit read-write mounting and
create-only text files. H563 write/sync/close/remount/readback passed, with
overwrite refusal, healthy watchdog and zero heap requests; see the storage
notes for the initial busy-release fix and retained test files.
For the STM32H5 SPI backend, manage each attached device's CS as a GPIO output,
not through the SPI peripheral's hardware NSS output. The backend owns GPIO
assertion/deassertion according to the portable transaction and error-cleanup
contract. For this SD-card fixture, that GPIO is PD14.

The H755 fixture uses SPI1 with PA5 SCK, PA6 MISO, PB5 MOSI (AF5), and PD14
GPIO CS. The same HSI/CKPER 64 MHz source yields 250 kHz startup and 1 MHz
reads. Board-selected `sd_board.h` supplies the bus adapter and MISO mapping;
the initialization, inspection and filesystem code is shared. H755's fixed
lwIP pools live in AXI SRAM to preserve DTCM stack headroom with this fixture.
Pin assignments follow the [STM32H755 datasheet](https://www.st.com/resource/en/datasheet/stm32h755zi.pdf).

H563 I2C has been validated with an MCP3425 on PB8/PB9 at 0x68, including
scan, conversion and interrupted-read recovery. An SSD1306 display is not
available; repeated START and physical clock stretching remain unqualified.

The portable API contract is in the
[SPI/I2C HAL section](platforms.md#spi-and-i2c-hal).

## Support and validation matrix

| Component | H563 | H755 M7 / M4 |
| --- | --- | --- |
| SPI/I2C HAL | IRQ adapters and optional SPI1 GPDMA payloads implemented; SD CRC-checked reads, read-only FatFs and temporary-file write/readback/remove tested; I2C MCP3425 scan/conversion and GPIO recovery tested | SPI SD initialization, CRC-checked 250 kHz/1 MHz reads, read-only FatFs and opt-in create/readback/remove HIL passed, including optional RX/TX DMA payloads; I2C hardware pending |
| Scheduler platform, TIM2, critical sections, sleep, reset | Implemented; hardware tested | M7 implemented; current console and reset paths hardware tested. M4 only performs boot synchronization and sleeps. |
| UART DMA/FIFO, USB CDC, board commands, shared console | Implemented; current HIL and earlier physical checks | Implemented; current UART/USB/TCP command and UART burst HIL. |
| lwIP Ethernet and TCP console | Implemented; hardware tested | Implemented; DHCP, large-packet ping, TCP reconnect and reset HIL. |
| A/B bootloader, flash journal, OTA integration | Implemented; hardware HIL plus host fault models | Implemented; 23-case HIL covers A/B OTA, journal rollover, watchdog/fault recovery and invalid-image fallback. |
| SD-file OTA | DMA A→B→A, byte-exact flash verification, unchanged flash during preparation, trial boots and confirmation tested | Same A→B→A checks tested with DMA and a compatible older package |
| IWDG, startup/trial health policy, retained fault handlers | Implemented; hardware HIL | IWDG1, startup health, and retained fault capture implemented and hardware tested; no IWDG early-warning IRQ. Bootloader starts IWDG; the application confirms healthy trial boots. |
| Real OTP and bank-B emulator | Implemented; emulator HIL, one authorized real write/lock, then read-only hardware checks | Not implemented; do not assume H563 OTP geometry or register semantics. |

Host/file models validate portable behavior; they do not qualify physical flash,
ECC, or brownout behavior. Physical power-cut testing remains deferred. See
[TODO.md](../../TODO.md) for outstanding qualification and [testing instructions](../testing.md)
for repeatable checks. Security work remains deferred.

## Remaining platform validation

The H563 and H755 M7 examples share the board console implementation: LED
control, button input, scheduler/DMA statistics, reset, UART echo, and formatted
logging. USART3 runs at 1 Mb/s on PD8/PD9 with interrupt-fed input and two
ping-pong TX DMA buffers (4 KiB each on H563, 8 KiB each on H755).
H755 needs the larger buffers for echoed input and log replies during 16-line,
maximum-length bursts; sustained input still requires pacing. H563 uses GPDMA1 Channel 0 and normal SRAM; H755 uses
DMA1 Stream 0 and AXI SRAM. Full buffers drop whole output frames and expose
counters. Both use internal HSI, TIM2 at 1 MHz, and shallow WFI sleep.
The shared STM32 board console includes `board timer <microseconds>`: a positive
unsigned decimal delay starts a one-shot DaveOS timer; its interrupt-time callback
logs `Timer fired` for deferred delivery. Reissuing replaces the pending timer.

H563 LEDs are PB0/PF4/PG4; H755 LEDs are PB0/PE1/PB14. Both read PC13.
H563's CPU runs at nominal 250 MHz, H755 M7 at 400 MHz. H563 hardware checks
passed for boot, UART bursts and TX DMA, USB/TCP commands, timer completion,
Ethernet, and software-reset recovery. Earlier physical checks also confirmed
all LEDs, button press/release, USB-C orientations, and cable reconnection.
Injected UART/DMA errors, precision timing, and prolonged sleep/backpressure
stress remain outstanding. H755 additionally boots M4 into sleep. Its current shared console has passed
UART/USB/TCP, timer, CRC, Ethernet, reset, and fault/watchdog recovery checks
after the static-storage, FIFO, board/composition, and convenience-API changes. H563
smoke tests also passed after the Application migration: UART/USB/TCP commands,
timers, statistics, button reads, LED acknowledgements, large-packet ping, TCP
reconnect, and software-reset recovery; no new physical LED/button confirmation. See TODO.md for
remaining work. Shared code changes require both board selections to build.

The application may call `platform.reset()` directly, independently of the
scheduler. STM32 H5/H7 request an immediate system reset without returning
(both cores on H755); no initialization, shutdown, or log drain is required.
Host/fake return `Status::unsupported` without changing state. Each STM32 board
module exposes this as `board reset` with no arguments.

## Networking and application composition

Both STM32 board selections use ST HAL and LAN8742, with
fixed DMA buffers and board-specific RMII wiring. H755 hardware validation
passed for DHCP/static IPv4, ping, cable reconnection, and USB console
responsiveness. H563 initial hardware validation passed for UART/USB commands,
TX DMA, LEDs/button, timer completion, reset, DHCP, ping, and TCP commands.
USB works in both USB-C orientations; USB/Ethernet recover after physical
reconnection. Its Cortex-M33 stack uses all remaining contiguous main SRAM,
with MSPLIM guarding the aligned end of static data. Long-lived STM32
application objects (modules, logger, scheduler, dispatcher, and transport
buffers) have file-scope storage rooted in `examples/stm32_console/appmain.cpp`.
Optional UART, USB, networking, and TCP components live in separate files selected
by Meson, with no application feature-selection preprocessor branches. A generated
`composition.hpp` assembles only selected members and their registration lists,
statistics routing, and shutdown calls; disabled components have no instances.
Component constructors are passive; hardware setup remains in module init.
Both boards build one `stm32-console` application target. `platform=stm32` and
`board=h563` (default) or `board=h755` select board files under
`platform/stm32/nucleo/`, the platform adapter, HAL, CPU/ABI flags, startup,
and linker script. Both use `meson/stm32.ini`; H755 additionally builds its
sleeping M4 image with separate CPU flags. Both
generated `Core/Src/main.c` entry points include `appmain.h` and call `appmain()`
after CubeMX peripheral setup; `appmain()` initializes the platform and calls
`application.run()`. Hardware setup waits for initialization;
Application binds command sources after both initialization stages succeed. The
old 64 KiB reservation addressed a 34,216-byte application stack frame before
objects moved to static storage. It now specifies minimum headroom only; the
stack uses the whole remaining RAM gap and no heap is reserved. H755 likewise
uses the remaining DTCM above static objects, with a 16 KiB minimum check. Both
boards reserve retained fault data and an emergency exception stack separately.
Startup stack painting, binary allocation reports and HIL counters are described
in [STM32 memory](../memory.md).

## Console library and device starter

Reusable USART3/USB transports, Board commands, Nucleo network wiring, and the
Console group live in `platform/stm32/console`, namespace
`daveos::platform::stm32`. The transport-agnostic serial module wrapper and TCP
console live in `console/`, namespace `daveos::console`. Library code depends on
core/console/net and a consumer-supplied board contract, never on example headers.
`TransportModule<Event, Transport>` owns a passive transport, initializes it in
stage1, and supplies the common command/log/statistics interface. UART and USB
expose the same shape; UART retains its FIFO and 16-line queue. A second active
USART3/USB instance is rejected; C callback routing pointers own no state.

`stm32::Console{uart, usb, tcp}` borrows any number of transports. It provides
modules(other_modules...), subscribers(), sources(), log_statistics(), and
reverse-order stop(). It creates no scheduler/module of its own and reserves no
names. Network services remain separately owned and outlive their TCP consoles.
The STM32 example's generated header selects objects; registration and console
shutdown use this library group.

Configured Nucleo startup, linker scripts, and peripheral glue live in
`platform/stm32/nucleo/{h563,h755}`. `stm32_support=true` enables their dependencies
without building the example. Export `daveos-nucleo`, `daveos-stm32-console`,
`daveos-stm32-uart`, and conditional `daveos-stm32-usb` Meson dependencies.
The board dependency propagates CPU/ABI and linker settings and compiles sources
in the consuming application. The consumer provides appmain.h/appmain(). H755
also provides a sleeping M4 image; programming must include both images.

`starters/stm32` is a separate Meson consumer with its own appmain(), periodic
Worker, and UART board console. Board selection defaults to H563. A mismatch
between the starter's board and the subproject's board is an error. Startup uses
our CubeMX configuration, not a board BSP. The host/fake and device starter wrap
pins must advance when new public APIs they consume are committed.

The documentation learning path has four levels: a small hello, logging and
commands, hardware console, and custom components. README links to these levels;
docs/reference.md retains detailed build, programming, and API information.
Validate actual host stdout/stderr/exit codes, common console registration and
reverse shutdown, existing USB/TCP behavior, and separate H563/H755 starter builds
and programming plans. Hardware claims remain distinct from build coverage.

The reusable full console has passed H563 UART/USB/TCP, Ethernet ping, timer,
and software-reset recovery checks. The standalone H563 starter has passed
periodic-worker, UART help/timer/statistics, and reset-recovery checks. The H755
full console also has current hardware coverage. Its standalone starter remains
build/programming-plan tested only.

## Flash layout and executable images

Reserve 32 KiB (four 8 KiB sectors) for the bootloader, including room to grow.
Measure the feature-complete bootloader at `-Os` and fail the build if it exceeds
that fixed reservation; never silently move the application slots. H563 main
flash uses this arrangement:

| Bank | Layout |
| --- | --- |
| 1, 0x08000000 | bootloader, metadata A (8 KiB), application A |
| 2, 0x08100000 | placeholder equal to bootloader reservation, metadata B (8 KiB), application B |

Metadata starts at 0x08008000 and 0x08108000; applications start at 0x0800A000
and 0x0810A000, each with 984 KiB capacity.

The H563 example's `bootloader=true` Meson option selects the slot-A linker
script and builds `factory.hex` with the bootloader, confirmed A (installation
counter 1), and identical initial journal records in both metadata sectors.
Its programming targets mass-erase main flash before writing and verifying;
the default standalone example remains available with `bootloader=false`.
The example also links B from the same compiled objects and validates the paired
relocation package during every bootloader-enabled build. Bring-up commands
`boot status` and `boot confirm` expose the executing slot and explicit durable
confirmation; they are omitted from standalone builds.
On H563, flash completion includes ICACHE invalidation before readback: the
application can otherwise read cached erased metadata despite a successful
physical write. Boot-time reads alone do not cover this condition because the
minimal bootloader leaves ICACHE disabled.

Application capacities and within-bank offsets are equal. No bank swapping is
used. Generate linker bounds, flash geometry and package metadata from one
layout definition. Enforce bounds before any erase/program operation. Metadata
A/B are a redundant journal of shared boot state, not exclusively per-slot
descriptors. Brief metadata-operation stalls are acceptable; bulk OTA must
yield to normal application work.

### SD-file update workflow

Reuse the existing OTA package format for SD files. The user mounts the
filesystem, runs `ota init <path>`, then `ota install`, and explicitly reboots
when installation succeeds, using the existing `board reset` command.
No automatic reboot is performed and no command rename is needed.

`ota init <path>` requires an already mounted read-only filesystem. Reject
an unmounted or read-write-mounted volume without changing its mode or touching
flash; explain that the user must mount read-only (unmount first if necessary).

`ota init <path>` cooperatively validates the complete package, including its
structure, target/layout compatibility and the reconstructed image CRC for the
destination slot, without erasing or programming flash. It prints package
information (version, target, size and destination) and prepares installation.
Successful preparation keeps the file open and reserves the filesystem. Close
the file after the last installation read, before submitting the final payload
to the engine; retain the filesystem reservation until installation finishes
or preparation is abandoned. This puts file read/close failures before commit. The prepared state has no
expiry: it may wait indefinitely for `ota install` or `ota disable`. Validation
and installation still use bounded I/O timeouts; the network inactivity timeout
must not expire an otherwise idle, prepared SD update. Other filesystem operations
return busy while reserved. A second `ota init` returns busy while a file is
prepared or installing; it never replaces the current selection. `ota disable`
abandons preparation and releases it before another file can be selected.

`ota install` uses the existing updater to erase/program the inactive slot,
verify the installed image from flash, and commit it for trial boot. Release
the file and filesystem reservation on success and on any preparation or
installation failure. Cleanup must wait for accepted I/O to release borrowed
buffers; a timeout alone does not establish that ownership has returned.
A failed preparation/installation is no longer prepared: another attempt
requires `ota init <path>` again. Failure cleanup does not imply that a damaged
or removed card is immediately usable without recovery/remounting.

`ota init <path>` itself authorizes the SD-file update; no preceding
`ota enable` is required. This does not enable network uploads, which retain
the explicit `ota enable` gate.

SD preparation and TCP uploads share exclusive updater ownership. The first
accepted `ota init` or TCP upload reserves it, including the SD validation
phase. An active TCP upload makes `ota init` return busy; SD validation,
prepared state and installation reject incoming TCP uploads as busy until
ownership is released. Merely enabling network uploads does not reserve the
updater. Failed admission must release any partially acquired reservations.

Extend `ota status` to report the update source, selected file path for SD,
phase (including validating, prepared, installing, done and failed), progress,
and last error. Successful preparation prints the image details once;
installation logs progress periodically rather than for every chunk. Keep a
copied diagnostic summary after success or failure without retaining the file
handle or filesystem reservation.

The reusable `update::FileUpdate` uses an injected `storage::ReadFile` source
and the existing flash engine. It has no FatFs, network or platform dependency.
The filesystem module provides a reserved read-only file adapter; file work
runs in an optional one-shot OTA task because FatFs reads can yield. The engine
continues to tick independently. `Module<Event, true>` adds `init`/`install`;
the default module retains network-only commands. Example composition enables
SD updates when bootloader, SPI SD and FatFs are selected, independently of
networking. Tests and hardware qualification are recorded in [storage](../storage.md).

### OTA package representation

Prefer one package containing a base image, block-local relocation records,
and the expected installed CRC for each slot. Link the same objects at both
addresses and verify reconstructed outputs byte-for-byte against independent
links during packaging. Apply relocations on-device in the transport-independent
updater. Reject unsupported transforms. Correct startup/vector-table handling
and prove execution from both slots on H563. If this is too complex or fails
validation, use separate slot-specific images with explicit destination and
load-address metadata. The initial `-Os` full-console study is build evidence
only: 202,088 bytes and 1,545 word patches reconstructed the second link exactly.

## Watchdog startup and application health

The demo starts the watchdog early enough to cover clock/peripheral setup and
module initialization, without depending on scheduler dispatch. Do not repeatedly
feed during initialization. Explicit init failure records diagnostics, attempts
bounded UART output, and resets; a hang expires the watchdog. One timeout covers
startup and normal execution; pre-start checks must be safe before module init.

Keep these named constexpr durations near the top of the application file:
watchdog timeout 5 s; health-check/feed period 100 ms; heartbeat period 100 ms;
heartbeat maximum age 1 s; repeating-task completion allowance 100 ms; trial
confirmation delay 5 s. Confirm after five seconds of healthy scheduler operation;
USB attachment, Ethernet link and DHCP are not confirmation requirements.

On H563, enable the IWDG early-warning interrupt with a nominal 128 ms
remaining at the demo timeout. Capture the interrupted basic register frame
using the reserved fault stack and retained CRC-protected record. Preserve an
already-latched health failure while adding its frame. Do not feed or return;
let IWDG reset. If a debugger is attached, break before waiting for reset.
Interrupt masking or an unpreemptible handler may prevent capture; watchdog
reset must remain independent. Freeze the scheduler clock with IWDG during
debugger halts to avoid false task-progress failures after resuming.

## Fault-handler integration

H563 Nucleo board support supplies the assembly handlers and enables configurable
fault exceptions before application initialization. CubeMX USER CODE weak
pragmas keep generated fallback handlers from replacing the assembly entry points.
The console reports fault identity, CFSR/HFSR, stack pointer and available frame
registers at the next boot; health clear explicitly removes the record.

## Implementation and validation gates

Implement CRC/version/flash/fault/watchdog foundations first, then the small
bootloader and measured layout, prove both slot executions, and add journal/OTA
integration. Power-loss tests interrupt every flash erase/program/commit boundary,
including journal rollover. Exercise corrupt images/metadata, trial rejection,
idempotent confirmation, older-version installs, replacement uploads and recovery.
Test malformed packages, relocation bounds, exact reconstruction, arbitrary TCP
fragmentation/coalescing, retries, abort/disable/disconnect/timeout, and reboot
refusal. Verify watchdog startup, latching, task rates, schedule changes, counter
resets, and retained diagnostics on host/fake.

On H563 repeatedly test A/B updates, rollback, faults, early init hangs/errors,
debugger/watchdog behavior, and UART/USB/TCP responsiveness during OTA. Measure
bootloader size and metadata stalls against the 100 ms progress allowance. Run
all repository tests, formatting, lint and existing H563/H755 builds; identify
hardware-tested results separately. H755 reliability HIL is separate from its
future bootloader/OTA integration.

## Test organization and board-specific HIL

Keep Catch2 for C++ tests. Run Python tests through pytest, including Meson-driven
host process and consumer checks. Hardware tests live in tests/hil/h563 and
tests/hil/h755 and are excluded from ordinary pytest collection and normal CI.
Explicit --hil selection requires a local configuration and matching firmware:
H563 bootloader/network/USB/TCP or H755 standalone/network/USB/TCP,
ST-LINK UART/SWD, USB CDC and reachable DHCP Ethernet.

Every selected HIL test starts with a main-flash mass erase, factory programming
and verification (both core images for standalone H755), clearing the retained
RAM fault record as well. There is no
reuse-installed-image option. A shared fixture
owns the board, serializes access, builds matching artifacts, captures transcripts
and controls OpenOCD/GDB. Missing prerequisites and provisioning failures fail
setup. Tests must be independent of execution order; cleanup closes transports
and resets the board even on failure. Store all generated artifacts under
build/. Watcher is a pinned test-only dependency for asynchronous text streams;
OTA binary protocol and byte-level UART stress retain appropriate direct I/O.

Cover all three console transports, reset/reconnect, Ethernet ping, UART bursts,
CPU fault frames, watchdog failures, startup trial rollback, and bidirectional
OTA with concurrent command traffic. Also cover PSP and invalid-stack capture,
interrupt-masked watchdog reset, faults with core debugging disabled, journal
rollover, both-images-invalid recovery, and OTA timeout/disable/reset/link-loss
interruption and replacement. PHY power-down and CPU reset tests do not replace
physical cable-unplug or power-interruption qualification. Watcher fixes belong upstream with tests;
update the pinned published commit after validation.

## OTP integration

After the host-file tests pass, implement a second test backend using flash in
the reserved bank-B placeholder. Use it to exercise the OTP module on H563 before
programming real OTP. This backend is part of the initial implementation plan.
Reserve one 8 KiB erase sector from the bank-B placeholder. Expose the same
32 logical 64-byte records as real H563 OTP; additional physical space within
that sector stores simulated permanent locks and interrupted-write tracking.
This allocation must not overlap boot metadata or either application slot and
must not change their sizes or addresses. The emulator's physical layout must
respect the underlying flash programming unit without reprogramming a unit to
simulate OTP's smaller writes. Records and simulated permanent locks survive
resets and OTA updates. Do not automatically erase the emulator during
initialization or when it becomes full. Existing factory programming clears it
as part of its main-flash mass erase; actual hardware OTP remains untouched by
factory programming. Keep emulated OTP programming and permanent-lock semantics
consistent with the host backend despite the underlying flash being erasable.

Real H563 OTP support requires coordinated MPU/cache configuration and NMI
handling. Use the reference OTP driver and interrupt handler below to guide the
hardware adapter. The reference distinguishes 16-bit programming units from
64-byte lockable blocks; do not equate these sizes or silently assume the same
geometry on H755. Keep device geometry in the platform adapter.

The NMI path must inspect FLASH ECC status and ECCDR, acknowledge ECCDETR as
required for reads of unwritten OTP, and preserve handling of genuine ECC errors
and unrelated NMIs. Integrate with the existing main-flash ECC handler rather
than installing competing NMI handlers. MPU configuration must coexist with
existing board memory attributes. Exact ECC classification and the hardware
sequence for applying and verifying permanent block locks require reference
verification; do not simply treat any ECC error as a free slot.

References reviewed:

- /home/david/form/g2/fw/src/common/form/hal/otp/
- /home/david/form/g2/fw/src/middleware/cube/Src/stm32h5xx_it_user.c
- ST guidance: https://community.st.com/stm32-mcus-60/handling-ecc-errors-in-stm32h5-series-reading-unwritten-otp-and-flash-data-area-143933
- Pinned STM32CubeH5 HAL: stm32h5xx_hal_flash.c (HAL_FLASH_OB_Launch),
  stm32h5xx_hal_flash_ex.c (OTP lock programming/current-state readback), and
  stm32h563xx.h (OTP geometry and ECC register fields).
- RM0481, FLASH OTP access/read operations and OTPBLR/ECC register descriptions,
  for the real-adapter qualification checklist; emulator success is not a
  substitute for verifying those hardware semantics.

### Bank-B flash emulator

Reserve 0x08100000..0x08101FFF, the first 8 KiB of the existing 32 KiB bank-B
placeholder. Generate/export the reservation from the same layout tooling as
boot metadata, with overlap/alignment assertions and programming-plan coverage.
The rest of the placeholder, metadata B at 0x08108000 and application B at
0x0810A000 remain unchanged. Require a compatible boot-layout build when this
backend is selected; do not assume a standalone firmware image reserves it.

Use 32 physical slots of 256 bytes each. A slot contains:

| Relative offset | Physical contents |
| --- | --- |
| 0..15 | Claim marker identifying format and logical block, with integrity check |
| 16..79 | 64-byte record body, with the type field left at 0xFFFF |
| 80..95 | Commit marker containing final type and record identity/integrity check |
| 96..255 | Ten independent 16-byte lock-attempt cells |

Write the claim first, then the body, then the commit marker. All-0xFF body
flash words carry no data and remain virgin; never program them merely as padding. Only a complete
valid commit marker supplies the type when reconstructing the logical record.
The service still sees the same 64-byte format and CRC convention; physical
markers are backend bookkeeping. A nonvirgin or ECC-damaged claim/body/commit
consumes the slot even when no valid record can be reconstructed. Unexpected
contents must never cause the emulator to erase itself.

Successful locking appends one valid lock marker tied to that block. Torn lock
cells are consumed and a retry uses the next virgin lock cell, without changing
the logical record or consuming another logical block. Any valid lock marker
makes the block permanently locked. Exhausting the ten lock-attempt cells returns
io_error with lock-phase diagnostics; do not rewrite a physical flash word or
erase the sector to recover. This bound is an emulator implementation limit,
not a limit imposed on the Store API or a hardware OTP lock operation.

Wrap the injected flash driver and poll only operations the emulator itself
started. Reject busy hardware before claiming a slot; never poll or clear another
owner's completion. Keep all waits bounded and leave interrupts enabled. The
emulator must coordinate with OTA/boot flash operations and use the existing
ICACHE invalidation/readback discipline. Do not feed the watchdog directly from
OTP code. The implementation bounds each flash-word wait to 100 ms and also
has a finite polling budget for a stalled clock. A timeout retains the write
buffer and makes that emulator instance unavailable until reset. Factory
main-flash mass erase is the only normal reset of this backend.

### H563 hardware adapter and ECC integration

Real-DUT policy: this board may already be provisioned. Scan read-only and report
existing blocks and locks before proposing any new record. Preserve unfamiliar
records. After the initial explicitly approved write/lock validation, all
automated real-OTP tests are read-only. Further real writes require explicit user
instruction. Repeated writes/failure injection remain on FileOtp/FlashOtp. The
HIL runner rejects builds with `otp_programming=true`.

On the qualified H563, ADDR_ECC identifies OTP 32-bit address groups as
`0x600 + (address - 0x08FFF000) / 4`; actual loads remain 16-bit. NMI delivery can
lag the load by several cycles. Keep the borrowed read context through ECC
status synchronization and acknowledgement, then remove it before returning.
Tests pin the region/bank/address matching and check both halfwords of every
OTP address group without broadening recovery to unrelated NMI sources.


Use the existing Nucleo MPU mapping (region 0, 0x08FFF000..0x08FFFFFF,
non-cacheable, execute-never) as the starting point. It is currently read-only.
The board owns the mapping: the driver must request a scoped programming access
change and restore permissions on every exit rather than resetting unrelated MPU
regions or globally disabling the MPU for a whole scan/write. Preserve access
to engineering/calibration bytes. Barriers accompany permission changes.

Read OTP through volatile 16-bit accesses only. Program length first, then every
remaining halfword other than type, and type last. Never program an OTP halfword
twice; 0xFFFF data still has programmed ECC state and must not be mistaken for
virgin storage. A locked blank block is consumed. A block with any programmed or
uncertain halfword is consumed. In the running process, retain consumption after
an attempted write even if subsequent readback fails.

Centralize recovery of flash ECC NMIs. During an explicit read, publish a narrow
borrowed read context identifying main flash versus OTP and the address/unit
being read; remove it before returning. Inspect ECCDETR's ECCD, OTP/region and
address information and capture ECCDR before acknowledging the event. The OTP
path uses the documented unwritten-halfword indication to classify virgin reads;
other recoverable errors become block read failures. The proposed virgin test
requires a matching OTP read, ECCD and the full 16-bit all-ones failing datum;
all halfwords in an unlocked block must qualify as virgin. A halfword read
successfully without that indication is programmed even when its value is
0xFFFF. Confirm this classification against the target before enabling real OTP
writes; the public service consumes ambiguous blocks. Do not copy the reference's
broad ECCDR low-byte truth test as a blanket exception suppressor. An NMI outside
the matching guarded read retains the existing fault capture/reset behavior.
Clear relevant stale ECC flags before the next read, preserve diagnostics, and
avoid logging or dereferencing the failing location from NMI context.

Peripherals cannot supply a historical record of a write attempt that leaves no
observable change. Do not claim that an all-ones data read alone proves a block
was never touched: classification must include ECC and lock evidence. Real-hardware
qualification must establish the supported interrupted-programming behavior;
ambiguous/damaged cells are never reused. Host failure injection does not replace
this qualification, and physical power-interruption testing remains deferred.

For locks, first finish and verify data programming, then update only the selected
OTP lock bit while preserving existing locks and unrelated option bytes. The
pinned HAL programs OTPBLR_PRG and applies options using OPTSTART; it reads locks
from OTPBLR_CUR. Verify the effective lock there before reporting success, and
restore flash/option-register locking on every exit. A PRG-register write alone
is insufficient. Do not add an implicit MCU reset to the setter. If hardware
requires a reset to make the lock effective, bring that constraint back for a
behavior decision instead of silently changing the synchronous contract.

The existing flash adapters and the new OTP adapter need shared controller
ownership for program/erase/option updates, including pending asynchronous OTA
operations. A separate driver object is not proof that the peripheral is free.
Busy rejection must not alter another owner's flags, permissions or lock state.
Test contention both while OTA writes B from A and writes A from B.

### Implementation and validation sequence

1. Add record encoding, Store, injected driver, diagnostics and unit tests.
   Cover empty storage, all slot boundaries, unknown/invalid/repeated records,
   read-only scans, unlocked valid records, full storage, exact deduplication,
   string validation, cache/view behavior, and all error paths.
2. Add FileOtp and reopen-based fault tests. Interrupt each halfword/commit/lock
   boundary; verify previous-value recovery or a valid unlocked new value,
   consumed slots, no data reprogramming, and lock-only retry. Test genuine ECC
   failure separately from expected virgin reads and fatal backend failures.
3. Add the module/commands and Meson composition. Test stage1 independence,
   stage2 consumers, count/confirmation errors, exact whitespace/case behavior,
   serial/status output, and logging-disabled operation.
4. Add the flash emulator and exercise it against FileFlash first. Test every
   claim/body/commit/lock boundary, physical write-once rules, retry exhaustion,
   layout overlap checks, resets/reopens, no erase-on-full and controller busy.
5. Add H563 HIL cases starting from the factory image: serial read/set/duplicate/
   mismatch, reset retention, A/B OTA retention, exhaustion, lock retries via
   test injection, and factory clearing. Verify adjacent placeholder bytes,
   metadata and both application slots are untouched by OTP operations. Preserve
   existing console, boot, OTA and watchdog coverage.
6. Integrate the real H563 backend with guarded ECC/MPU/controller handling.
   Exercise classification and register-operation seams in host tests, then
   validate non-programming hardware access before any deliberate OTP write.
   Real fuse/lock qualification and physical power-cut evidence remain distinct
   from emulator success; do not claim them from a passing file/flash suite.

No new tick-driven state machine is needed for these synchronous operations.
If implementation introduces one, use core::StateMachine and the AGENTS.md
single-transition structure. Keep named constants for geometry, formats and
operation timeouts at the top of their relevant files. Run the full supported
host/fake/sanitizer, ARM, formatting/lint and selected HIL checks before pushing.

## H755 reliability binding

The shared Health module uses the board-selected reliability adapter. H563 and
H755 configure watchdog/debug freeze, consume reset flags, record initialization
failures, and perform bounded emergency UART output/reset through their own
platform implementation. Constructors remain passive; `app_early_init()` starts
health checks and IWDG before clock/peripheral setup on either board.

H755 M7 owns IWDG1; M4 remains asleep. Unlike H563, H755 IWDG has no early-warning
interrupt. A detected health failure is retained and feeding stops; an arbitrary
hang or interrupt-masked stall resets without a pre-reset frame. TIM2 and IWDG1
freeze together when M7 is halted by the debugger. Standalone builds report
confirmation as not configured and do not claim an image was confirmed.

Reserve the first 2 KiB of M7 DTCM for a 256-byte retained record followed by the
emergency exception stack; exclude it from startup data/BSS initialization.
Assembly fault entries preserve the original MSP/PSP before switching stacks.
Validate stacking status, alignment, and RAM bounds before reading core registers;
these registers precede optional floating-point storage. DTCM bypasses the M7
cache. Publish the retained record in complete 32-bit stores, with magic last:
H755 bring-up observed byte-written magic lose its high byte at reset before
startup ran. Full-word publication is covered by fault/reset HIL.

`health crc "text"` compares hardware, software and split incremental
CRC-32/ISO-HDLC results on both board selections. H7 enables CRC through AHB4;
interrupt callers of the CRC service continue to use its software fallback.

Bootloader/OTA is integrated on both boards using the layouts below. OTP remains
an H563 integration; no H755 OTP provisioning is part of this work.

### Agreed H755 A/B layout

Keep the sleeping M4 image at its existing boot address and preserve equal M7
application slots. Each bank has eight 128 KiB erase sectors:

| Bank | Sector 0 (128 KiB) | Sector 1 (128 KiB) | Sectors 2–7 (768 KiB) |
| --- | --- | --- | --- |
| 1 | M7 bootloader at 0x08000000 | Metadata A at 0x08020000 | Application A at 0x08040000 |
| 2 | Sleeping M4 at 0x08100000 | Metadata B at 0x08120000 | Application B at 0x08140000 |

The bootloader keeps a 32 KiB code-size limit initially; the rest of its erase
sector stays reserved. The M4 image is fixed during OTA and can be replaced only
through factory programming. It is excluded from M7 OTA packages. OTA must never
erase either bank's sector 0. No bank swapping or boot-address option-byte
changes are planned.

The H755 integration covers the M4 boot handshake, 32-byte flash program
units, commit-marker isolation, M7 cache maintenance, and bounded metadata work
while the application keeps running.
Existing H563 layouts and persisted formats must remain compatible. The layout
and fixed-M4 policy are implemented; qualification limits are tracked in TODO.md.

### H755 journal responsiveness

Preserve the existing 100 ms task-progress allowance during runtime OTA and
metadata operations. Do not relax health checks or feed the watchdog blindly to
hide flash stalls. Journal reclamation must respect the executing application's
bank: no runtime erase of that bank's metadata sector. Schedule inactive-bank
erases cooperatively, avoid reads from a busy bank, and preserve a verified,
committed snapshot elsewhere before erasing the latest copy.

Without an executing-bank constraint, the journal retains the H563 policy:
append to the newest sector and switch when full. H755 supplies that constraint:
append in the inactive bank; before reclaiming it, copy and verify the latest
record into a free checkpoint entry in the executing bank. During boot, prepare
checkpoint space for the selected application by preserving the latest record
in the other bank, then erasing the selected application bank's metadata sector
only when it is not blank. Bound scans and programming work too;
asynchronous erase alone does not establish responsiveness. Verify the timing
budget in both application slots, including journal rollover, with console and
health tasks running. Preserve crash consistency at every new checkpoint and
erase boundary in host fault-injection tests.

If no safe runtime journal operation is possible within these constraints,
return an explicit update error and leave reboot under application control.
Do not silently erase the executing bank or reboot to make space. Preserve the
last committed metadata and follow the existing failed-update eligibility rules;
an error must not make a partial image bootable.

### H755 boot handoff analysis

The current M7 application waits for M4 to enter STOP, configures clocks, then
releases M4 through HSEM. The bootloader must not perform that handshake and
then let the application repeat it. Leave M4 in its initial wait until
the selected application initializes it. Before handoff, disable both USART3 and
its APB clock: the boot UART clock otherwise keeps D2 awake and makes the
application's normal M4 STOP check fail. Preserve the standalone application's startup
path and avoid persistent handoff flags when hardware state suffices.

Start IWDG1 early in the H755 bootloader, before peripheral initialization and
image selection/verification, so cold-boot hangs also reset. Carry the running
watchdog through handoff; the application must adopt it without assuming reset
state or an interval with watchdog protection disabled. Bootloader feeding must
reflect bounded forward progress, and application feeding must retain the
agreed health checks. This is not a separate trial-only watchdog. Waits for
flash completion or UART output must have deadlines rather than feed forever.
Preserve reset-cause evidence and the retained fault record through handoff so
the application can report the preceding failure. Verify debugger freeze and
watchdog expiry in the bootloader as well as during application initialization.

### H755 A/B implementation sequence

1. Extend host/file-backed flash and journal tests for the H755 geometry and
   executing-bank constraint before adding hardware writes. Support 32-byte
   programming, with the commit marker in a separately programmed final unit.
   Keep H563's existing 16-byte programming and persisted journal format
   compatible; version any distinct H755 record format explicitly in both
   firmware and factory tools. Test interrupted checkpoint/erase/commit,
   exhaustion, reboot recovery, and both executing banks.
2. Add the injected H755 flash backend, cache maintenance and flash-error/ECC
   handling. Enforce region ownership, reject runtime erases in the executing
   bank, and keep bootloader/M4 sectors outside application write permissions.
   Preserve the current CRC, watchdog and retained-fault integrations.
3. Generate both slot linkers, layout descriptors and package metadata from the
   agreed layout. Build the M7 bootloader with the 32 KiB size check and create
   a factory image containing bootloader, fixed M4, initial journal records and
   confirmed application A. Preserve the standalone build. Use existing H563
   boot eligibility, installation ordering, trial/confirmation and CRC rules.
4. Validate factory boot, watchdog-protected handoff, M4 startup and execution
   from both M7 slots before enabling network OTA. Check vector relocation,
   UART/USB/TCP, timers, health and retained faults in both slots.
5. Wire the existing cooperative TCP OTA protocol and uploader to the H755
   layout/backend. Validate paired-image reconstruction, writes/readback CRC,
   confirmation, failed-trial/corrupt-image fallback, and repeated A/B installs
   while console and health tasks keep running. Exercise journal reclamation
   and safe exhaustion explicitly; measure stalls against the 100 ms budget.
6. Extend board-selected HIL to factory-provision H755 A/B images and retain
   standalone coverage. Run host/sanitizer/fake tests, H563 and H755 builds,
   formatting/lint, programming plans and H755 hardware qualification. Keep
   physical power-cut testing separately deferred and do not program real OTP.

The implementation follows these stages. See TODO.md for completed validation
and remaining qualification; this sequence is not a claim of full qualification.

### H755 journal format and flash timing

Both journal formats occupy 256 bytes. H563 keeps format v1 unchanged, with CRC
at byte 236 and a 16-byte commit trailer at byte 240. H755 uses format v2, with
CRC at byte 220 and the final 32-byte commit unit at byte 224. The commit unit
contains the marker, duplicated sequence, complementary marker and 16 zero
padding bytes; validate all of it so a partially programmed trailer cannot
masquerade as a complete duplicate. The decoder accepts both versions; new
records use the platform's write granularity. Factory tooling agrees byte for
byte with these encodings.

Scan identifying headers newest-first and fully validate only records that can
replace or conflict with the newest validated snapshot. This bounds normal
foreground scan work even when a sector is full. Free-entry searches inspect
the whole candidate before reuse; ECC/read errors make an entry unavailable.
The flash backend protects both sector-zero reservations, rejects active-bank
erases and active-image programming, and serializes instances until the owner
polls completion. Explicit validation reads guard data BusFaults to inspect ECC
status; unrelated faults retain the normal fault/reset path. Cache invalidation
after mutation precedes readback. Programming uses the controller/memory barriers
required before writing each 32-byte flash word.

The layout supplies a flash-operation timeout: one second on H563, five seconds
on H755. This bounds cooperative flash waits and does not change the 100 ms
scheduler-progress allowance. Host console round-trip timing and watchdog task-progress checks assess runtime
responsiveness; they do not constitute direct CPU-stall measurements. Host models
cover interrupted checkpoint/commit/erase and exhaustion; hardware verifies
normal reclamation while commands and watchdog health checks continue.
