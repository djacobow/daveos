# SPI and I2C

The allocation-free `daveos-hal` dependency provides controller/master SPI and
I2C transactions. It is separate from the scheduler: applications inject bus
backends, synchronization, and timing. SPI uses eight-bit words; I2C uses
**unshifted seven-bit addresses from 0x08 through 0x77**.

## Device driver interface

A portable driver holds a borrowed `hal::spi::Device` or `hal::i2c::Device`.
It need not know the concrete controller or the application's alias enum.

```cpp
#include "hal/spi/action.hpp"
namespace hal = daveos::hal;
namespace spi = hal::spi;

// These objects belong to the driver and survive the entire transfer.
std::array<std::uint8_t, 1> command{0x9f};
std::array<std::uint8_t, 3> reply{};
std::array actions{spi::write(command), spi::read(reply)};
spi::Completion completion;

// `device` is supplied by application wiring.
auto status = completion.start(device, actions, std::chrono::milliseconds{10});
// Later, from a task:
if (auto result = completion.result()) {
  // result->status, result->completed_actions, result->actions
}
```

Alternatively, pass `spi::Callback::bind<&Driver::Completed>(*this)` to
`device.start()`. `Completed(const spi::Result&)` runs in interrupt context,
possibly before `start()` returns. Rejected submissions never invoke it. The
polling helper publishes immediate rejection as a ready result and never loses
an early completion. One task owns each helper; an interrupt publishes its
result. Reusing a pending helper returns `busy` without replacing its result.

The caller owns descriptors and buffers until completion. Do not change TX data
or descriptors, or access RX data, while pending. The callback receives the
original references and may reuse them to start another transaction immediately.
An exchange requires equal-length, nonoverlapping TX/RX buffers.

SPI actions are `write`, `read` (fill byte defaults to 0xff), `exchange`, and
`pause(chrono_duration)`, and `check_response(bytes, expected, mask, idle)`.
The response check scans at most 32 previously received bytes, skips leading
idle bytes (default 0xff), and compares the first response under the mask
(default 0xff). Mismatch or no response ends the transaction with
`response_mismatch`; subsequent actions never start. It performs no bus I/O.
CS stays asserted across the list, including pauses.
`idle_clocks(80)` is a standalone transaction: MOSI high, CS inactive, positive
multiple of eight clock cycles. Idle clocks count as a transaction, not a
read/write action. There is no internal transaction queue, retry or cancellation.

I2C actions are `write` and `read`. Every action starts an addressed phase,
including repeated START between consecutive writes or consecutive reads. Only
the last action ends with STOP. Register addresses, if needed, are ordinary
bytes in the caller's write buffer.

## Application wiring

`hal::Controller<Backend, Clock, Critical, DeviceCount>` owns one physical bus's
state and device configurations. Its constructor is passive. Call `init()`
before using its handles; with the DaveOS clock, start transfers only after the
scheduler is running.

Use `controller.device<0>()` after construction, or the static
`ControllerType::bind<0>(controller)` when wiring references without accessing
an unconstructed dependency. A handle remains borrowed: all controllers,
registries, backends, clocks and callback targets must outlive their operations.
Quiesce interrupts before destroying any of them.

```cpp
enum class DeviceId { flash, display };
hal::Registry<spi::Action, DeviceId::flash, DeviceId::display> devices{
    std::array{spi1.device<0>(), spi2.device<0>()}};
auto flash = devices.device<DeviceId::flash>();
```

Aliases are checked at compile time. Use `hal::BusRegistry` to validate all
controllers before hardware setup and roll back partial initialization:

```cpp
enum class BusId { external, sensors };
hal::BusRegistry buses{hal::bus<BusId::external>(spi1),
                       hal::bus<BusId::sensors>(spi2)};
auto status = buses.init();
// On failure: buses.initialization_failure() provides the entry index/status.
auto& external = buses.controller<BusId::external>();
```

Tables reject duplicate physical controller identities, duplicate SPI CS pins,
and duplicate I2C addresses on one controller. Separate controllers can use the
same I2C address. Initialization and registry wiring are task-context operations
performed before transfers are allowed.

`hal::DaveOsClock` in `hal/adapters/daveos.hpp` adapts scheduler timers. It can be
constructed with a platform reference alone and bound to a scheduler during
initialization, avoiding constructor prerequisites. Budget up to two pending
DaveOS timers per active controller, in addition to other application timers.
Timer exhaustion rejects a new request or aborts an accepted one safely.

Timeouts use exact, positive chrono durations. The automatic timeout is the
sum of pauses plus `max(10 ms, 4 * estimated wire time + 1 ms)`. Reset defaults
to 100 ms. Fractional microseconds and arithmetic overflow are rejected.
Controller `reset(callback, optional_timeout)` is asynchronous and does not
cancel active work: it returns `busy` while a transaction is pending.

Statistics are task-context-only synchronized samples of saturating 64-bit
counters; fields do not form an atomic multi-counter snapshot. Device and
controller snapshots/clears have independent epochs. Cleanup failure preserves
the original error and faults the controller until reset succeeds. The
controller's `faulted()` flag and cleanup/reset counters expose this condition.

## STM32 integration

Include `platform/stm32h5/bus.h` or `platform/stm32h7/bus.h`. These supply
`SpiBus`, `I2cBus`, `SpiConfig`, `I2cConfig`, `BusHardware`, and `BusCritical`.
The backends use CMSIS register definitions and bounded interrupt handlers;
they do not require the vendor SPI/I2C HAL source files.

The application supplies the peripheral pointer, IRQ numbers, GPIO-CS device
configuration, and clock configuration. `BusHardware::prepare` sets up clocks
and pins during task-time initialization. `reset` performs a bounded RCC reset
pulse, including after partial initialization; it must not poll or invoke
callbacks. For I2C, `idle` samples SCL and SDA. I2C also requires a board-validated
TIMINGR value and effective bus rate. Attached I2C device initialization remains
the application driver's responsibility.

SPI selects a prescaler whose actual frequency does not exceed the requested
maximum. Supply the actual kernel clock frequency. GPIO chip selects are under
the backend's control. Larger SPI and I2C actions are split into hardware-sized
chunks without changing their public action boundaries.

Route the selected SPI IRQ, or both I2C event/error IRQs, to
`controller.interrupt()`. Use the same preemption priority as the timer IRQ;
the backends select the lowest priority, matching DaveOS TIM2. Different IRQs
must not concurrently execute this controller's completion delivery. IRQ wrappers
refer to explicit application-owned controller instances; there is no global
active-bus singleton.

Cleanup uses a peripheral reset rather than the vendor's polling SPI abort.
This stops IRQ-driven access to borrowed buffers without waiting. An I2C device
holding a line low leaves the controller faulted; the current backend does not
pulse SCL to release a stuck slave. An explicit reset rechecks the lines.
DMA is not implemented. SPI DMA remains a separate follow-up.

## H563 SD fixture and validation

Enable `-Dspi_sd_probe=true` with `-Dboard=h563 -Dexamples=true`. The optional
module uses SPI1 on PA5 SCK, PG9 MISO, PB5 MOSI, and PD14 GPIO CS. Its kernel
clock is HSI/CKPER. Startup uses 250 kHz, and sector verification also uses
1 MHz (below the card's CSD maximum). The application must not assign these
pins or change CKPER to another source while using the fixture.

Run `sd probe` on a console. It performs the SD v2 startup sequence, reports
OCR, reads CSD/CID (capacity, maximum clock, manufacturer ID, product, revision,
serial and manufacture date), and uses CMD17 for single-sector reads. The
current fixture requires SDHC/SDXC with CSD v2 and 512-byte block addressing;
older SDSC cards are reported as unsupported.

Every register/data block is checked with SD's CRC16. Sector 0 and each ordinary
primary partition's first sector are read three times at 250 kHz and three
at 1 MHz, comparing all 512 bytes against the slow baseline. MBR partition
ranges and overlaps are checked against card capacity. FAT12/16/32 is identified
from validated BPB geometry and cluster count, not a textual filesystem label.
A filesystem directly in sector 0 is also recognized. exFAT is reported as a
signature hint only; GPT and extended partition traversal are not implemented.
Unknown or malformed layouts are reported without attempting to mount them.

The `sd probe` command remains a read-only diagnostic. No sector
writes, formatting, mounting, directory traversal or filesystem consistency
checks occur within the probe. Optional `-Dfatfs=true` adds a separate
[filesystem worker](storage.md) with `fs mount/ls/read/unmount`. The fixed SPI action-list API keeps CS asserted for the command
and data capture. A 13,024-byte module-owned receive buffer accommodates 100 ms
of token-wait clocks at 1 MHz plus framing and a sector. Each operation clocks
the whole capture window, even if the card answers early. Transfers are
asynchronous but deliberately inefficient; this is not the eventual high-speed
SD/FatFS implementation. A private fixture backend config copy allows speed
changes only between completed operations without introducing a general HAL
reconfiguration API.

`tests/sd_inspect/` tests CRC vectors, corrupt/truncated/error responses, CSD
capacity overflow, FAT type boundaries, invalid BPBs, partition bounds and
overlap. The formats follow the [SD physical-layer specification](https://www.sdcard.org/downloads/pls/)
and [Microsoft FAT specification](https://www.scs.stanford.edu/~zyedidia/docs/_other/fat.pdf).

`tests/hil/h563/test_spi.py` requires this build option and a connected card.
Like other HIL cases, it factory-provisions main flash first. The inspection
and read-only filesystem cases never write the card or real OTP. A separate
file-creation case requires explicit opt-in; see [storage](storage.md). Five consecutive complete inspections passed on H563:
60 sector reads with matching CRCs/data at both rates, followed by healthy
watchdog and empty retained-fault checks. The connected card reports:

- Product `SE32G`, manufacturer ID `0x03`, OEM `SD`, revision 8.0, date 2024-10.
- 30,436 MiB capacity; maximum clock 25 MHz (only 1 MHz tested).
- One MBR partition, type `0x0c`, starting at LBA 8192, 62,325,760 sectors.
- FAT32 BPB, 32,768-byte clusters, 973,584 clusters (the inspection itself does not mount).

The expansion passed host 30/30, ASan/UBSan 30/30, formatting, cppcheck
(including explicit inspection-header checks), an H563 A/B firmware build,
and the SPI HIL case above. This does not qualify other SPI modes, high-speed signal
integrity, arbitrary sectors, or filesystem contents. H755 bus adapters remain
build-tested only. I2C physical validation awaits a device fixture.

Fake backends in `platform/fake/bus.hpp` accept borrowed scripts and record
fixed-capacity traces. `BusClock::take_due()` separates timer selection from
invocation so tests can deliberately deliver a stale alarm. Invoke callbacks in
the fake platform's interrupt domain. The fake controller lock and publication
tests also exercise concurrent host threads.

Prior HAL validation, before the SD inspection expansion: host 29/29, ASan/UBSan 29/29, TSan 28/28,
fake 23/23, logging-disabled 30/30, and networking 30/30 Meson checks passed.
The HAL executable contains 19 cases; allocation checks also cover the bus path.
Both ARM configurations build, including explicit SPI/I2C template instantiation.
GCC 13 consumer compilation, formatting, and cppcheck (including explicit HAL
header checks) passed. Portable pytest reported 21 passed and five skipped.

The full H563 HIL run reported 27 passed, five OTP skips, and one watchdog-test
failure: it injected a counter fault into task index zero, which the new SD
module changed. The test now finds `health.Heartbeat` by module/task name.
The corrected watchdog case and the SD case both passed on that firmware.
This is a corrected targeted rerun, not a claim of a second complete HIL run.
H755's corresponding test uses the same lookup but was not rerun on hardware.
