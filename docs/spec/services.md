# Utilities, functions, and portable services

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

These components sit outside the scheduler. Their service contracts remain
portable through dependency injection. The currently supported hardware bindings
are listed in the [STM32 support matrix](stm32.md#support-and-validation-matrix).

## Contents

- [Optional networking](#optional-networking)
- [Bootloader, OTA, and reliability services](#bootloader-ota-and-reliability-services)
- [OTP storage (agreed behavior and implementation design)](#otp-storage-agreed-behavior-and-implementation-design)

## Optional networking

Networking lives outside core in `daveos::net`, with pinned lwIP 2.2.1 in
NO_SYS mode. An application-owned service takes a driver and monotonic clock;
a thin CRTP module polls every 1 ms, processing at most four received frames
and servicing stack timeouts. PHY link polling occurs every 250 ms. Interrupts
never call lwIP. All packet and stack allocation uses fixed preallocated pools.
The first milestone supports Ethernet/ARP, IPv4, ICMP ping, UDP for DHCP, and
DHCP or application-configured static addressing. TCP provides a single-client
nonblocking byte-stream server for the console. IPv6, DNS, fragmentation, TLS,
and a general multi-listener/connection API are deferred. Future application
networking uses this separate library rather than extending SchedulerInterface.
Hardware-init failure leaves the remaining application operational and requires
reset to retry. Cable and DHCP recovery are automatic. `net status` reports link,
addressing, and counters. Meson `networking` defaults to false and disabled builds
need no networking submodules.

The optional TCP console is an independent log subscriber and command source,
like UART and USB, and listens on port 1000 by default. A second connection is
reset while the first remains active. All commands use the existing dispatcher;
TCP callbacks only buffer bytes. Disconnect, peer FIN, or link loss discards
partial input and queued output; half-close is not supported. The next client
starts a fresh session. Fixed 4 KiB RX and 8 KiB TX buffers bound storage. RX uses
TCP flow control; output overflow drops a complete record without blocking.
Disconnected output is not retained. The protocol is plain TCP, without Telnet
negotiation, authentication, or encryption, and relies on local terminal echo.
Omitting the `tcp` feature removes the console without disabling networking, UART, or USB.


Shared transport-agnostic console helpers belong in `lib/console/`, namespace
`daveos::console`, with an explicit Meson dependency. `BufferedOutput` serves
asynchronous UART/USB transmission. A common CRTP console module handles task
scheduling, dropped-line reporting, command logging/dispatch, and independent
source/subscriber registration; transports retain session, echo, and I/O rules.
Prefer application ownership and borrowed callback contexts over singleton
objects. HAL/middleware APIs without user context may use a minimal callback
routing pointer, with teardown quiescing hardware before detaching it. lwIP's
global stack constraint does not justify a generic singleton guard.
TCP processes at most one completed line and 256 input bytes per invocation.
It consumes spans from a ring buffer without shifting remaining bytes. Bytes
after a newline remain for subsequent commands, partial lines persist between
invocations, and CRLF may cross chunk boundaries. No additional TCP command
queue is required.

## Bootloader, OTA, and reliability services

Bootloader/OTA integration supports H563 and H755 M7, with host/fake tests.
H755 keeps the factory-installed M4 image fixed during M7 OTA. Components are optional,
allocation-free, and use injected platform services. Standalone applications
remain supported; boot control reports `not_supported` without a bootloader,
and OTA is unavailable. See the [STM32 flash layout](stm32.md#flash-layout-and-executable-images)
and [platform/file flash contract](platforms.md#injected-flash-and-host-file-backend)
for hardware integration and simulation.

### Versions, compatibility and persistent boot state

Application and bootloader have independent version stamps containing uint32_t
major/minor/build, Git commit ID, and dirty-tree flag. Major/minor are defined in
the application's top-level meson.build. CI supplies build through a Meson
option; local builds use reserved UINT32_MAX and display `local`. Omit timestamps
by default for reproducibility. Bootloader major/minor use the boot_version_major
and boot_version_minor options; version_build applies to both components. The
board version command prints application identity; the bootloader prints its own
identity on UART. CI checks generated stamps and the packaged OTA identity against
its build number. Images carry an application-defined product ID
and flash-layout revision; updater and bootloader reject mismatches. Use a
versioned image format with space for future signing; v1 integrity uses CRC only.

The journal stores complete boot-state snapshots in append-only CRC-protected
records. Program the commit marker last in its own flash programming unit.
Preserve a valid committed snapshot while reclaiming either region. A separate
record sequence orders journal snapshots. A device-local uint64_t installation
counter advances only when a fully verified installation commits; aborted
uploads do not consume a number. Preserve it when replacing pending images or
erasing the inactive slot. Reject counter exhaustion rather than wrapping.

Boot preference follows installation order, not firmware version. Reinstalling
identical or older firmware is a new installation. New OTA installations get
one trial. Durably mark the trial started before jumping; any reset before
confirmation, including power loss before the jump, rejects that installation.
Do not automatically retry it. The application explicitly applies its health
policy and calls confirm_image(). Confirmation is bounded and synchronous:
success means durable confirmation has been verified. It is idempotent without
another write when already confirmed. Errors are returned for application retry.

Read and CRC-verify the full selected image from flash on every boot, including
confirmed images. A failed CRC prohibits booting it. Try another eligible image
only after validating it; rejected trials remain ineligible. If neither image
is eligible/valid, or no committed metadata state is recoverable, record the
failure, attempt bounded UART diagnostics, wait one second, and reset.

ST-LINK installation erases all main flash and installs bootloader, a confirmed
application, and fresh metadata/history. OTP is untouched. Ordinary OTA never
rewrites the bootloader. Emergency application-driven bootloader rewriting is a
future possibility, not a safe or implemented v1 update path.

The bootloader stays small. Reuse DaveOS services where helpful, including the
logger with explicit draining. UART only, matching application UART/baud, with
bounded best-effort output; UART failure never blocks a valid boot. No USB,
networking or command dispatcher is required in the bootloader.

### CRC service

Use CRC-32/ISO-HDLC matching Python binascii.crc32() for chunks, images, and
metadata. Polynomial 0x04C11DB7 (reflected 0xEDB88320), initial internal register
0xFFFFFFFF, reflected input/output, final XOR 0xFFFFFFFF; `123456789` checks to
0xCBF43926 and empty input checks to zero. External incremental state follows
binascii's initial-zero convention. Each caller owns its state. Foreground
updates may use injected hardware; interrupt callers use software without
waiting for the peripheral. Updates synchronously process caller-bounded byte
spans, accepting arbitrary lengths/alignment. Test incremental partitions,
interleaved callers, and exact hardware/software agreement.

### Watchdog and application health

`watchdog::HealthModule<Event, Platform, Hardware>` (`lib/watchdog/health.hpp`) is
the scheduler module applications register: heartbeat and task-progress checks,
retained failures, trial-image confirmation and the `health` commands. Hardware
is a policy type with static reliability services (watchdog driver, retained
fault record, failure recording, CRC backend); `platform/stm32/console/health.hpp`
supplies the Nucleo one as `stm32::Health<Event>`. Host tests use an in-memory
policy (`tests/health`).

Provide an optional generic watchdog module with injected hardware support
(STM32 IWDG). Start explicitly. Named application-provided checks run before
hardware enable and before every task-driven feed. First check/feed failure
latches its identity/status and prevents further feeds until reset, even if the
condition recovers. Failed pre-start checks also latch without enabling hardware.
Timeout and check/feed period are explicit chrono durations. Configure IWDG to
freeze while a debugger halts the CPU.

In addition to the independent heartbeat, check every active repeating task's
completed count against its actual cadence: every iteration due more than the
completion allowance ago must have completed. Expose scheduler progress data to
an optional health helper, handling initial delays, cancellation, period changes,
rescheduling, statistics resets, and the checking task's unfinished invocation.
Resetting diagnostic statistics must not hide progress deficits. Latch failures
with task/module identity and expected/actual counts. This policy is optional
application health checking, not an unconditional core scheduler overload policy.

### Retained fault diagnostics

Reserve one fixed RAM record shared by bootloader/application, excluded from
startup clearing. Begin with magic, then format version, size, data, and CRC.
Store image identity and copied names/data, not pointers into an old image.
Latest failure wins. The application reads and explicitly acknowledges/clears
the record; the bootloader preserves it. Reset retention is supported; power-loss
retention is not promised.

Capture HardFault, MemManage, BusFault, and UsageFault frames/status registers.
Validate frame accessibility and avoid logging from fault context. Reject frames
when exception stacking or a hardware stack-limit check failed, even when the
reported stack pointer is inside RAM. Preserve fault status without copying an
invalid frame. Record first,
then break if debugger control is enabled; otherwise reset. Resuming that
breakpoint proceeds to reset. Watchdog failures use the same retained record.

### OTA engine and streaming

Keep three distinct state-machine levels: transport chunks (initial maximum
1 KiB, one outstanding, CRC checked before use), package blocks with local
relocation records, and platform flash operations. Arbitrary transport boundaries
may split package records or contain multiple pieces; retain unconsumed bytes.
Do not buffer the whole image or relocation table.

Durably invalidate the destination before its first erase. Erase only sectors
needed by the incoming image, one at a time as required; wait for completion
before programming that sector. Program in hardware write units and yield
between bounded steps. Pad a final partial write unit with 0xFF, excluding that
padding from the declared image CRC. Finally read flash incrementally to verify
the expected installed CRC before committing eligibility/installation counter.

OTA is disabled initially and application-controlled. The demo exposes
`ota enable`, `ota disable`, and `ota status`; enablement is not retained across
reset. Require a confirmed running image before accepting uploads, preserving
its fallback while a trial runs. Accept unlimited replacement uploads before
reboot; replace only the inactive slot. Disabling an idle updater does not
invalidate an already completed installation.

The host polls readiness, sends an offset/chunk/CRC, and waits while the target
processes it. Advertise limits and expected offset; no blocking flash work in
status handling. Reject a bad chunk CRC before programming and allow resending
that chunk. Flash erase/program errors or final readback CRC mismatch abort the
installation. Disconnect, explicit abort, disable during upload, or inactivity
timeout aborts without resume. New attempts restart from the beginning. An
in-flight hardware operation may finish before storage is reused. Host inactivity
timeout defaults to configurable 30 s, including partial chunks; device-side
erase/program/verification time does not count against it.

Use versioned binary framing with explicit bounded lengths, fixed byte order,
and sequential offsets on a dedicated configurable TCP port (default 1001).
Allow one client/session and provision lwIP for OTA plus the existing port-1000
console. Binary OTA never travels over the console UART. Future console base64
or SD-card adapters feed the same transport-independent engine.

A Python uploader takes target address and image path and reports progress and
errors. Optional --reboot sends a protocol request only after successful install
commit. An application-provided callback accepts/declines it; decline leaves the
image installed/pending and is reported distinctly. Reboot does not confirm it.

## OTP storage (agreed behavior and implementation design)

Implementation status: the record codec, Store, optional module/commands and
persistent host FileOtp and injected bank-B FlashOtp emulator are implemented.
See [OTP usage guide](../otp.md) for composition and test usage. H563 emulator HIL
covers factory/reset retention and bidirectional OTA. The real H563 OTP backend
is implemented with provisioning disabled by default. Guarded read/NMI behavior
has been tested on a previously provisioned H563. One explicitly authorized
write stored `dave_nucleoh563_sn001` in block 1 and verified its permanent lock;
all other blocks retained their fingerprints and locks. Read-only firmware was
restored and HIL verified retention through factory programming and reset.
Further automated real OTP tests are read-only. Physical power-cut qualification remains deferred.

Provide an optional OTP service outside the scheduler core, with injected
platform storage and CRC support. Constructors remain passive. During stage1,
the module initializes its injected backend, scans storage and populates
fixed-size RAM storage. It has no initialization dependency on other modules;
its backend and CRC support must be usable within its own stage1. Its stage2 is
a no-op. After successful stage1, the cache is available to every module during
stage2 without depending on module registration order.

Begin with a persistent host file backend, following the FileFlash approach, so
development and repeatable tests consume no real OTP capacity.

Read and write APIs are restricted to the scheduler thread, including module
initialization callbacks. No interrupt-context or concurrent host-thread access
is supported. Writes synchronously program and read back one block, verify the
record, and return the result; no background task or asynchronous completion API
is required for these infrequent provisioning operations.

### Records and cached access

Treat the available OTP storage as ordered, fixed-size slots, with exactly one
record per hardware block. On H563 this gives 32 slots of 64 bytes each. Each
record structure occupies the entire block; do not pack multiple records into
one block or span a record across blocks. Each record has a type identifier,
size, and CRC32 header followed by its payload. The H563 record layout is:

| Offset | Field | Size |
| --- | --- | --- |
| 0 | Type identifier: enum class with std::uint16_t underlying type | 2 bytes |
| 2 | Payload length: std::uint16_t, counting payload bytes only | 2 bytes |
| 4 | CRC32: std::uint32_t | 4 bytes |
| 8 | Payload | 56 bytes |

The record totals 64 bytes. CRC32 uses the existing DaveOS CRC32 convention
and covers the type, payload length, and all 56 payload bytes, in stored order,
excluding the CRC field itself. Fill unused payload bytes with 0xFF before
calculating the CRC using the intended final type identifier.

Reserve type 0xFFFF as invalid/unwritten. Program the length, payload, and CRC
before programming the type field last as the completion marker. A record is
accepted only when its completion marker, length, CRC, and supported payload
validation pass. A type field still at 0xFFFF does not prove the block is unused:
partially programmed contents elsewhere in the block consume it. Unknown types
other than 0xFFFF remain subject to the skip-and-consume policy below. Readback
verification after the final type write is required before reporting success or
updating the RAM cache.

Permanently lock each successfully programmed and verified block as part of the
write operation; locking is not deferred to a separate provisioning API. Verify
the lock before returning success for a newly written record. Each record uses
an entire block, so there is no remaining space in that block to preserve for
future writes. If record verification succeeds but permanent locking fails,
return an error and expose the verified new value in the RAM cache. The block
remains consumed. This keeps runtime reads consistent with the valid record
that initialization would find after reboot; do not fall back to the previous
value solely because locking failed.

Initialization scans every potential slot, validates records, and loads valid
supported data into RAM. For a repeated type, the valid record later in physical
slot order replaces the earlier cached value. An invalid later record must not
replace a valid earlier value. Unknown record types do not fail initialization:
skip their contents and treat their blocks as consumed, preserving compatibility
with data written by newer firmware. Initialization also accepts a valid but
unlocked record, for example after a reset between programming and locking.
Cache its value, block identity and lock state, but do not program or lock OTP
during initialization. A later identical write retries only that block's lock.
Startup scanning is read-only with respect to OTP contents and permanent locks.
Record validity and precedence depend on contents, CRC and physical slot order,
not the lock bit. A valid unlocked record is usable; a locked corrupt record is
not. Consult lock state for locking/verification and to exclude locked blocks
from write candidates; being unlocked does not establish that a block is unused.
Public read accessors use the RAM cache rather than rereading OTP.

If a block has a genuine recoverable ECC read error, record the problem, treat
the block as consumed, and continue scanning. It must not replace an earlier
valid cached record or become a write candidate. Failure to initialize or access
the backend as a whole fails module initialization. Expected ECC indications
from never-written OTP are distinct from genuine per-block read errors.

For every supported record type, compare a requested write with the current
valid cached record of that type. If the type, payload length, and payload bytes
are identical and its block is locked, return success without programming or
consuming a block. If the valid cached record is unlocked, whether from a failed
lock operation or the initialization scan, an identical write retries only the
permanent lock on that existing block and verifies it. Do not rewrite data or consume another slot; return
success once locking is verified, otherwise return the lock error and retain
the verified cached value. These rules apply to all types, not only serial
numbers, and require no free slots. Compare contents rather than relying on
CRC equality. Deterministic padding and CRC make identical content an identical
stored row.

Changed records append immediately after the highest consumed block, starting
at block zero when none have been consumed. Normal operation must use consecutive
blocks and create no gaps. A partial or failed write consumes its block rather
than creating a reusable gap. If externally produced contents contain an earlier
unused gap, do not backfill it: physical slot order must continue to represent
write order. Records never update a previously written record in place.
Distinguish never-used storage from a used slot containing invalid or partially
written data. A partially written or corrupt
block is consumed permanently and must never be reused. Initialization skips
such records and retains the latest earlier valid value for each type.

After programming a record, read it back and verify it before updating the RAM
cache. If verification fails, return an error, preserve the previous cached
value, and leave the affected block consumed. A subsequent write uses a fresh
block. If a changed value needs a new record and no never-used block remains,
return core::Status::full without changing the RAM cache. Identical writes
consume no storage when all blocks are used, and succeed if the existing block
is already locked or the lock-only retry succeeds.

The first and only implemented payload type is a serial number: ASCII text up
to the maximum payload capacity. Device keys and additional types remain future
work. On H563 the serial number may occupy all 56 payload bytes. The payload
length field gives its character count; no NUL terminator is stored. The RAM
accessor returns std::optional<std::string_view>, referring to the RAM cache.
If no valid serial-number record exists, initialization still succeeds and the
accessor returns std::nullopt. The application decides whether provisioning is
required. Accept 1 through 56 printable ASCII characters
(bytes 0x20 through 0x7E inclusive), including spaces. Preserve characters exactly;
do not trim whitespace. Reject empty strings, overlength values, control
characters (including NUL and DEL), and non-ASCII bytes before programming or
consuming a slot. Apply the same serial-number validation during initialization
before accepting a stored record into the RAM cache.

### Demo commands

Provide commands to show the cached serial number and report the backend,
consumed/remaining slots and error counts. Setting the serial number uses an
explicit serial set subcommand with two required string arguments: the proposed
serial number and its confirmation. Compare the parsed strings exactly,
including whitespace and case. If they differ, return core::Status::invalid_argument
without programming data, retrying a lock, consuming a slot or changing the RAM
cache. Matching values must still pass the normal serial-number validation.
There is no single-argument setter shortcut.

The setter is otp serial set "ABC 123" "ABC 123". The command namespace is
otp, separate from ota firmware updates. The read/status commands are otp serial
and otp status. The serial command adapter handles the explicit set form as
described below, using the existing dispatcher tokens.

### Implementation design

Keep the storage service in daveos::otp (otp/), independent of the scheduler,
logger, commands and STM32 HAL. Provide otp::Module<Event = core::NoEvent> as a
thin optional DaveOS adapter. It owns no peripheral or global singleton: the
application constructs a backend, a Store with a borrowed driver and optional
CRC service, and then the module referring to that Store. Constructors only
store configuration/references. Module stage1 calls Store::init(); stage2 does
nothing. No periodic task is needed.

Use the existing borrowed context/function-pointer DI idiom, without virtual
methods or heap allocation. The initial supported geometry is 32 blocks of
64 bytes; an incompatible backend fails initialization before any mutation.
Keep the geometry reported by the backend so another target's adapter cannot
silently inherit H563 assumptions. Do not implement H755 OTP by assuming it is
identical to H563.

The driver provides these synchronous operations:

- init(): prepare/open the backend and validate its geometry.
- inspect(block, result): return the 64 logical bytes, lock state and an enum
  describing unused, consumed/readable, or consumed/unreadable storage. A
  recoverable block error is represented in that result; a non-ok operation
  status means the backend could not reliably inspect storage.
- program(block, record): program one fresh logical record, with the type field
  committed last. Return status and an attempted flag. Busy/rejected operations
  that never touch storage do not consume the next block. Once programming may
  have started, a failed attempt consumes it. No automatic data-write retries.
- lock(block): idempotently apply the permanent lock. Store then inspects the
  block to verify the actual lock state; issuing a lock request is not proof.

The backend owns physical programming order and the distinction between virgin,
programmed and unreadable storage. The Store owns record validation, CRC, append
position, equality checks and the RAM cache. There is no erase or arbitrary
reprogram operation in the public OTP driver. It does not expose raw mapped OTP
pointers to callers.

Use explicit little-endian encoding rather than writing a compiler-dependent
struct representation. Define serial_number = 1 in the record type enum and
reserve 0xFFFF as the uncommitted value. Other identifiers are unknown types.
Verify the header offsets and 64-byte total at compile time. Check payload
length before using it; validate CRC over bytes 0..3 and 8..63, then validate
the supported payload. Require canonical 0xFF padding. All-zero, malformed,
uncommitted and checksum-invalid records are consumed but not cached.

Store exposes init(), ready(), serial(), set_serial(string_view), and snapshot().
Before successful initialization, serial() returns nullopt and set_serial()
returns not_running; ready() distinguishes this from an initialized device with
no serial number. Repeated successful init returns already_initialized; an init
failure leaves the service unavailable. Returned serial views borrow fixed RAM
storage and are valid until the next verified value change or Store destruction.
Lock-only retries and identical no-op writes do not invalidate a view. Never
return a view into driver staging storage or mapped OTP.

Stage a proposed value before mutation, including when it aliases the current
cached string. Invalid input and full/busy rejections leave cache and append
position unchanged. A failed programming/readback attempt leaves the previous
cache intact and consumes its target when the backend reports an attempt. A
verified new value replaces the cache before lock completion; lock failure is
reported separately without hiding that value. A reset during an operation may
leave a valid unlocked record; the next scan is authoritative.

Use existing core::Status values: invalid_argument for bad values/confirmation,
full for exhausted append space, busy for unavailable shared hardware, io_error
for read/program/lock failures, timeout for bounded operation timeouts, and
checksum_error for mismatching record verification. Invalid backend geometry or
host-file format is incompatible. Status snapshots identify the failing phase
(scan, program, verify, lock), block and status so an io_error is diagnosable.
Error returns after mutation are explicitly not rollback guarantees.

Snapshots use fixed-size value types, including backend identity, readiness,
consumed blocks, remaining appendable slots, invalid/unknown/unreadable block
counts, latest record's block/lock state, and saturating operation-error counters.
Report an anomalous earlier gap as unavailable for append, not free capacity.
An expected unwritten-OTP ECC indication is not counted as a genuine read error.
Counters are diagnostic data, not a tick-driven state machine.

### Command adaptation and composition

Keep the command dispatcher unchanged. Register otp serial as a raw-arguments
command adapter, with a normal typed no-argument otp status command. Serial
accepts exactly zero arguments (read) or three arguments (literal set, value,
confirmation). Validate this grammar and exact string equality before calling
Store::set_serial. All strings come from the existing dispatcher tokenizer;
there is no second splitting/quoting implementation. Do not accept a single-value
shortcut. Help explicitly prints both forms and the confirmation requirement.

The adapter is the only raw command boundary; service accessors/setters remain
typed. Include quoted spaces, escaped quotes, and maximum-length serial numbers
in command tests. Two maximally escaped 56-byte serials fit in the default
256-byte line buffer and the command uses five tokens, within the default eight.
Keep logging/command availability orthogonal to the Store and backend.

Use Meson composition files to select none, host file, H563 flash emulator or
H563 hardware backend; never silently fall back between them. Demo H563 testing
selects the emulator explicitly. Real OTP programming is a separate deliberate
qualification step, after host and emulator coverage. H755 OTP is outside this
initial adapter implementation. File paths and injected objects outlive their
borrowers and remain valid for file-scope construction.

## Filesystem service

The optional `lib/storage/` component owns no hardware. Its injected block-device
contract supplies readiness, capacity, synchronous reads and an exclusive mount
lease. An adapter may implement a synchronous read by pumping asynchronous HAL
completion with task-only yield. Accepted I/O must complete or time out before
borrowed buffers are released. All volumes are accessed on one execution thread;
same-volume re-entry fails immediately rather than waiting.

FatFs is a pinned submodule, configured without heap allocation. Mounts are
read-only by default; an explicit read-write mount requires injected write/sync
callbacks. New-file creation never overwrites an existing file. Removal requires a
read-write mount, rejects directories, holds the volume guard across lookup and
unlink, and reports success only after metadata synchronization.
Application-owned volumes attach explicitly to a fixed C-ABI drive registry.
The optional module serializes requests in a one-shot worker and returns between
output records. Paths are copied before command dispatch returns. The initial
commands mount, unmount, list, preview, create and remove files; FAT12/16/32 and 512-byte sectors
are supported. See [storage](../storage.md) for limits, configuration and tests.

### TCP file transfers

An optional, independent service on port 1002 supports directory listing, file removal,
mkdir, empty-directory removal, and create-only uploads to
read-write mounts and downloads from read-only or read-write mounts. Listing
also permits either mount mode; all mutations require rw. Listings stream one
entry per request, retaining the lease until end or cleanup. It reserves
the filesystem for one transfer, uses bounded 1024-byte stop-and-wait chunks,
and reports size and CRC32. Upload success requires sync and close. Disconnect,
timeout or failure closes and removes an incomplete upload before releasing the
lease; failed cleanup is reported and leaves any remaining file for explicit
removal. It never overwrites on retry. The protocol, lifetime rules and Python
client are specified in [TCP file transfers](../file-transfer.md).

### SSD1306 display

The reusable `drivers::Ssd1306` driver takes a HAL I2C device and owns a fixed
128×64 framebuffer. Requests progress through an asynchronous state machine;
callers tick it until completion and retain the driver throughout pending I/O.
Drawing is rejected during updates. The optional STM32 `ssd1306` feature adds
explicit initialization, text, test-pattern and clear commands, sharing the
I2C module's lease and diagnostics. See [SSD1306](../ssd1306.md).
