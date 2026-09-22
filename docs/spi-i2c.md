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
`poll_response(window, expected, mask=0xff, fill=0xff)` repeatedly clocks a
borrowed 1–32-byte receive window until its **last** byte matches under the mask.
It keeps CS asserted and requires an explicit transaction timeout; the deadline
never restarts. Each window counts as a read attempt, but successful polling
completes one logical action. Backend errors abort immediately.
`read_until(response, idle=0xff, maximum_bytes=0)` reads one byte at a time
until it differs from `idle`, preserving subsequent payload bytes for the next
action. The response span must contain exactly one byte. An explicit transaction
timeout is required; a nonzero byte limit also bounds the search, returning
`response_mismatch` on exhaustion. A zero limit relies on the deadline.
CS stays asserted across the list, including pauses and polling.
`idle_clocks(80)` is a standalone transaction: MOSI high, CS inactive, positive
multiple of eight clock cycles. Idle clocks count as a transaction, not a
read/write action. There is no internal transaction queue, retry or cancellation.

I2C data actions are `write` and `read`. Every action starts an addressed phase,
including repeated START between consecutive writes or consecutive reads. Only
the last action ends with STOP. Register addresses, if needed, are ordinary
bytes in the caller's write buffer.

`controller.probe(i2c::Address{0x68}, callback, optional_timeout)` probes any
valid seven-bit address without registering it or changing configured devices.
It uses the same controller exclusion, deadline, IRQ callback and cleanup rules
as other transactions. The address is copied; completion references a static
one-action descriptor. Probe counters belong to the controller, not a registered
device. A standalone `i2c::probe()` action can also target a registered device
through its ordinary handle. It sends an address in the write direction, then STOP,
without a data byte. Completion reports `ok` for ACK or `nack` for an absent
responder. Other errors remain errors, not evidence of absence. Probes count
as transactions but not read/write actions; ordinary empty reads/writes are
still invalid. Do not combine a probe with other actions. Scans exclude the
reserved address ranges, and a successful probe identifies a responder, not
its device model.


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

`hal::DaveOsClock` in `lib/hal/adapters/daveos.hpp` adapts scheduler timers. It can be
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
This stops IRQ-driven access to borrowed buffers without waiting. A low I2C line leaves the controller faulted after cleanup. There is no
implicit transaction retry. Explicit asynchronous `reset()` can recover the bus
when the board supplies `I2cRecoveryPins::operations()` as the optional final
`I2cBus` constructor argument. These pins must be the bus's actual, exclusively
owned SCL/SDA pins, with clocks/pulls/alternate functions prepared by board code.

Recovery disables the peripheral, temporarily selects open-drain GPIO, sends
up to nine recovery clocks if SDA is held low, then attempts STOP and checks
both lines. All outputs either pull low or release; they never drive high.
Low/high/setup/bus-free phases last at least 5 us; the controller advances the
machine with timer notifications at least 10 us apart, so other tasks and
interrupts continue. A stretched/held-low SCL waits for release within the
original reset deadline (default 100 ms). Nine clocks without SDA release
returns `faulted`; a deadline or timer-arm failure returns `timeout` or
`timer_error`. Every exit releases GPIO and restores the alternate function.
A healthy bus needs no recovery pulses. See [NXP UM10204, bus clear](https://www.nxp.com/docs/en/user-guide/UM10204.pdf).

The backend's bounded `reset()`/`finish()` and the board RCC-reset hook remain
nonblocking register operations. Optional `start_reset()`, `poll_reset(now)` and
`abort_reset()` hooks supply the longer recovery sequence to `Controller`.
The separate reset deadline remains armed throughout; failed recovery leaves
the controller faulted. Without supplied GPIO hooks, reset only reinitializes
the peripheral and checks idle, as before.

With recovery hooks, a low line at `init()` leaves an initialized but faulted
controller, so recovery can run after scheduler timers become available.
`needs_reset()` exposes this backend condition. The I2C command module reserves
the buses during initialization and queues startup recovery for its first task;
no transfer or GPIO pulse runs during constructors or module initialization.
Other users of the HAL must explicitly submit reset when `faulted()` is true.
I2C remains interrupt-driven. The shared SPI backend accepts an optional injected
DMA engine; without one it retains the FIFO/interrupt path. H755's `Spi1Dma`
uses DMA1 stream 1 RX and stream 2 TX (DMAMUX requests 37/38), with FIFO enabled
and byte-sized transfers. Reserve both streams and route their IRQs to the
same controller as SPI1. UART's stream 0 remains independent.

Actions of at least 32 bytes use DMA in chunks of up to 512 bytes. Smaller
commands, token polling and tails use interrupts. The engine owns aligned TX/RX
staging in AXI SRAM: application spans may still reside in DTCM, which DMA1
cannot access. TX is copied/cleaned before starting; RX is invalidated/copied
only after both streams and SPI complete. Completion/timeout cleanup disables
DMA requests, stops both streams, deasserts CS and resets SPI. A failed stop
faults the controller; private staging prevents DMA retaining borrowed caller
buffers. Do not destroy/reuse the engine or its staging until it is quiescent.
H563's `Spi1Dma` uses GPDMA1 channel 1 RX and channel 2 TX (requests 6/7),
with byte-sized single beats and two private 512-byte buffers in ordinary
nonsecure SRAM. UART retains channel 0. Both DMA IRQs route to the SPI
controller. Stop suspends an enabled channel and waits for quiescence before
resetting that channel, with bounded polling; it never resets the DMA controller.
See [RM0481](https://www.st.com/resource/en/reference_manual/dm00733993.pdf)
for the GPDMA suspend/reset sequence.

## Nucleo SD fixture and validation

Select the `sd` feature with `-Dexamples=true` and `-Dboard=h563` or
`-Dboard=h755`. Both use SPI1 on PA5 SCK, PB5 MOSI and PD14 GPIO CS.
MISO is PG9 on H563 and PA6 on H755, selected by the board's `sd_board.h`.
The H755 AF5 assignments are listed in the [ST datasheet](https://www.st.com/resource/en/datasheet/stm32h755zi.pdf).
The kernel
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
checks occur within the probe. The optional `fatfs` feature adds a separate
[filesystem worker](storage.md) with `fs mount/ls/read/unmount`. The fixed SPI action-list API keeps CS asserted for the command
and data capture. `lib/storage/sd/read.h` builds a shared sequence that reads R1
(with an eight-byte response bound), waits for the data token, rejects bad
responses before payload transfer, then reads only the payload and CRC. The
module's receive buffer is 516 bytes; the transaction deadline still bounds
an absent response. Both boards default to DMA for sector payloads; set
Omit the `sd-dma` feature to compare the interrupt-only path. It applies to both boards.
`sd stats` reports SPI/DMA IRQ entries, backend polls, DMA chunks and bytes
since startup (32-bit counters wrap; compare unsigned deltas). Poll counts are
not a CPU utilization measurement. A private fixture backend config copy allows speed
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
integrity, arbitrary sectors, or filesystem contents. The subsequent H755
bring-up passed the same five inspections and the read-only filesystem HIL
case (mount/list/error handling/remount), with zero heap attempts and 3,480
observed stack bytes. A subsequent H755 opt-in create/readback/remove HIL
cycle passed with 4,064 observed stack bytes and zero heap attempts; see
[storage validation](storage.md#validation). Other SPI modes, higher rates,
media removal and write power-loss behavior remain unqualified.
H563 I2C has the MCP3425 fixture described below; H755 I2C remains build-tested
only.

## H563 MCP3425 fixture

Select the `i2c-adc` feature with the H563 example. Connect I2C1 PB8 SCL,
PB9 SDA, common ground and suitable power to the MCP3425; external pull-ups
must bring both bus lines high. The pins use AF4 open-drain, with no internal
pull-ups. The fixture selects HSI 64 MHz for I2C1 and conservative Standard-mode
TIMINGR `0xf0421317` (SCL below 100 kHz; waveform/rise-time qualification is not
claimed). Do not assign PB8/PB9 or I2C1 to another component simultaneously.

Commands:

- `i2c scan`: address-only probes of `0x08`–`0x77`; a 16-column, 8-row ACK map
  uses spaced hexadecimal column labels, `*` for ACK and a blank otherwise.
  Reserved positions remain blank.
  Each configured bus has its own named map. Bus errors abort that bus’s scan
  explicitly; scanning continues with the remaining buses. Only one scan or
  sample runs at a time.
- `i2c reset`: asynchronously recover every configured bus and report a named
  outcome for each; active client leases cause `busy`.
- `adc sample`: start one 16-bit, gain-1 conversion and log signed raw code and
  integer microvolts. No scheduler task blocks while waiting.
- `i2c stats`: named read/write/transaction/error counters for every configured
  bus. Values above `UINT32_MAX` display as `4294967295+`; counters retain
  their full 64-bit values.
  Scan NACKs count as failed transactions, so they are expected in this output.

`lib/hal/adapters/i2c_module.hpp` provides `hal::I2cModule<Event, Buses...>` over
borrowed named bus adapters. The H563 composition passes only `I2C1`; adding
another adapter includes it in both scan and stats without changing the command
handlers. The module initializes the buses and reserves them for a scan,
rolling back partial reservations on `busy`. ADC sampling holds a task-time
lease through the conversion and all HAL completions, so scanning cannot change
the client sequence between transactions. `adc` is a separate module with only `sample`;
it does not own interrupt routing or bus initialization.

`lib/drivers/mcp3425.h` provides the board-independent `drivers::Mcp3425` reader
through the `daveos-drivers` Meson dependency (which depends on `daveos-hal`),
using an injected `i2c::Device`. Its constructor is passive. One task calls
`request()`, then `tick(monotonic_microseconds)`, and inspects `result()`.
The reader owns its transaction buffers, starts a one-shot with `0x88`, checks
configuration readback, and polls ready at five-millisecond intervals. Bus
transactions have a ten-millisecond timeout; the conversion budget is 250 ms.
An expired conversion never releases buffers still owned by a HAL transaction.
There are no automatic error retries. Keep the reader alive until completion.

The fixture uses **seven-bit address `0x68`**, encoded on the wire as `0xd0`
(write) or `0xd1` (read). Sample scaling is 62.5 uV/LSB, truncated toward zero
for integer microvolts. See the [Microchip MCP3425 datasheet](https://ww1.microchip.com/downloads/aemDocuments/documents/OTH/ProductDocuments/DataSheets/22072b.pdf).
The reference driver in the Form repository informed the selected mode; its
singleton and vendor-specific I2C API are not used.

`tests/mcp3425/` exercises signed endpoints, repeated conversions, busy
responses, configuration mismatch, write/read NACKs, transfer and conversion
timeouts, and buffer lifetime. `tests/hil/h563/test_i2c.py` factory-provisions
main flash, scans three times and takes 30 conversions, then checks watchdog,
retained faults, stack and heap diagnostics. It neither writes the SD card nor
programs OTP. The host recovery suite covers SDA release after 0/1/3/9 clocks, failure after
nine clocks, delayed/held-low SCL, timer failure, abort at each waveform phase,
reset reuse, probe/device address isolation, and per-controller counters.
Repeated START, physical clock stretching, higher bus rates and ADC accuracy
against a calibrated source still require separate hardware qualification.

Initial ADC hardware validation, before the GPIO-recovery follow-up, passed three scans (only `0x68` acknowledged) and 30
conversions around 1.630 V, with zero transaction timeouts, healthy watchdog,
no retained fault and no heap requests. Stack painting observed 2,968 bytes after separating the modules;
this is not a worst-case bound. The 333 failed transactions were expected NACKs
from the 111 absent addresses over three scans. Host ASan/UBSan passed 33/33;
targeted TSan HAL/ADC tests passed 2/2, followed by a passing ADC/diagnostics
rerun after the module split. Two-bus host tests cover named output, scanning
every address, reservation rollback and continuing after an individual bus
error. H755 was not flashed for this change.

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

Recovery follow-up validation: host 32/32, ASan/UBSan 33/33, TSan 32/32,
fake 26/26 and logging-disabled 34/34 passed, plus formatting and cppcheck. Both H563/H755
A/B builds pass. A new H563 HIL case deliberately interrupts an MCP3425 read,
checks the ADC is holding SDA low while MCU outputs are released, and exercises
explicit and startup recovery. Both H563 HIL cases passed: three scans and 30
conversions with zero transaction timeouts, followed by successful explicit
and startup recovery of the interrupted read. Subsequent samples and scans
worked, with a running watchdog and no retained failure. Stack painting in
the scan/conversion case observed 2,808 bytes and zero heap attempts; this is
not a worst-case stack bound. No SD or OTP writes were performed. H755 was
not flashed; physical clock stretching and repeated START remain unqualified.

## Timeout cleanup and explicit recovery

A failed or timed-out transaction completes exactly once after bounded
cleanup. SPI disables DMA requests/interrupts, stops both DMA directions,
deasserts every CS and resets the peripheral. Completion returns the borrowed
buffers even when cleanup fails: DMA adapters must isolate them with private
staging. A cleanup failure faults the controller and rejects later starts;
only an explicitly requested successful controller `reset()` clears that fault.
Successful cleanup makes the bus reusable but does not establish the attached
device's protocol state. Neither the HAL nor the SD layer retries transfers.

SD I/O failure invalidates card readiness. Application policy decides whether
to attempt recovery; an interrupted write has an uncertain outcome and must
not be replayed automatically. Release the filesystem/OTA reservation, request
`sd reset` (SPI controller only), then `sd probe` (one card initialization and
read-only inspection), and finally `fs mount`. Reset and probe reject mounted
or otherwise leased media. A failed probe leaves the card unavailable; it may
require a physical card power cycle. No automatic recovery clocks or commands
are issued after a failed SD transfer.
