# FAT storage

Enable `-Dfatfs=true` and initialize the pinned submodule:

```sh
git submodule update --init storage/fatfs
meson configure build/host -Dfatfs=true
meson test -C build/host storage --print-errorlogs
```

`daveos-storage` provides an application-owned `storage::Volume` and optional
`storage::Module<Event>` command worker. Both receive a borrowed
`storage::BlockDevice`; neither depends on STM32 or a particular bus.
`daveos-file-block` supplies a host file backend opened read-only by default (`open(path, true)` opts into writes).
The source is FatFs R0.16 patch 2 from the Zephyr mirror, pinned as a submodule.
Only its generic filesystem and Unicode sources are compiled; no Zephyr runtime
or disk driver is used.

The configuration supports FAT12/16/32, 512-byte sectors, UTF-8 long filenames,
four volumes, and 32-bit sector addresses. There is no heap allocation or formatting API. Mounts default to read-only;
new-file creation requires an explicit read-write mount. exFAT is not enabled. Long-name work buffers use the
calling stack. Paths are volume-relative, at most 255 bytes; drive prefixes
and embedded NULs are rejected.

## Ownership and cooperative execution

Construction only stores dependencies. `Volume::attach()` explicitly reserves
a numbered drive. FatFs's C disk API has no context argument, so a small fixed
registry maps drive numbers to borrowed block devices; it does not own a
singleton volume or card. Destruction releases registration. The device must
outlive its volume, and destruction must wait for operations to unwind.

Mounting acquires the device until unmount. The volume permits one open file or
directory. Calls use one execution thread; nested access to the same volume
returns `FR_LOCKED` immediately. Do not bypass the wrapper with direct FatFs
calls. This prevents a yielded task from waiting on its suspended ancestor.

The block device's read operation is synchronous from FatFs's perspective.
The SD adapter starts an asynchronous HAL transfer and calls
`scheduler().yield()` while it is pending. Each yield runs at most one eligible
task. Interrupts continue normally. An accepted transfer must finish or time out
before borrowed buffers can be released, even when the scheduler is stopping.
A fake adapter must advance time in its pump; yield itself does not advance time.
See [the yield contract](yield.md).

The command module copies arguments and schedules a one-shot worker. It returns
between directory entries and 16-byte preview records so logging/events can
drain. Slow filesystem work is not registered as a periodic watchdog heartbeat.
Commands return acceptance status immediately; completion/errors are logged later.
One request may be active at a time; another returns `busy`.

## Nucleo SD example

Use SPI1 with GPIO-controlled CS: H563 uses PA5/PG9/PB5 for SCK/MISO/MOSI;
H755 uses PA5/PA6/PB5. Both use PD14 for CS. Select `-Dboard=h563` or
`-Dboard=h755` with `-Dspi_sd_probe=true -Dfatfs=true`.
See [SPI/I2C](spi-i2c.md) for the bus contract.

```sh
meson configure build/spi-h563 -Dspi_sd_probe=true -Dfatfs=true
meson compile -C build/spi-h563
```

After programming, issue:

```text
sd probe
fs mount
fs ls
fs ls "some directory"
fs read "some file.txt"
fs read "some file.txt" 512 128
fs unmount
```

`sd probe` initializes and validates the card before mount. A mounted volume
rejects another probe until unmounted. Listing stops after 256 entries.
Read emits hex/ASCII, defaults to offset zero and 128 bytes, and accepts 1–4096
bytes per request. It never sends binary file contents directly to a console.

The initialized-card transport uses CMD17 and verifies each sector's CRC16.
It reads R1 and the data token incrementally, then clocks only the 512-byte
payload and CRC. The shared `storage/sd/read.h` sequence needs a 516-byte
buffer, retains CS across all phases, and rejects a bad response before reading
the payload. A missing token is bounded by the transaction deadline. H755 uses
DMA for sector payloads by default (`-Dspi_sd_dma=false` selects interrupts);
H563 also uses DMA by default, through GPDMA1 channels 1/2 and private SRAM
staging. Both still use 1 MHz, single-block transfers.
Multiblock transfers and higher-speed qualification remain future work.
Only SDHC/SDXC cards are supported.
On a read failure, unmount and run `sd probe` again before remounting.
There is no hot-plug automount or card-detect pin contract.

### Reusable SD initialization

`storage/sd/initializer.h` provides `daveos::storage::sd::Initializer`,
independently of FatFs or an application module. It uses `daveos-hal` and the
shared state-machine helper; construction only stores a SPI device handle.

```cpp
namespace sd = daveos::storage::sd;
sd::Initializer initializer{spi_device};
// After power has settled, configure mode 0, 8-bit MSB-first, SCK <=400 kHz.
auto status = initializer.request();
// In a periodic task, while retaining exclusive use of the card:
initializer.tick();
if (auto result = initializer.result()) {
  // Check result->status; result->ocr and result->command aid diagnostics.
}
```

`request()` rejects overlap with `busy`; `result()` remains empty until the
accepted operation completes, and remains available until another request.
Keep the initializer and bus alive and stationary until completion. No method
waits for an interrupt; HAL callbacks only publish transfer completion.
The sequence supplies 80 clocks with CS inactive, CMD0/CMD8,
CMD55/ACMD41 until ready, and CMD58 to check power-up and SDHC/SDXC addressing.
Commands are separated by eight clocks with CS inactive. Each HAL transfer has
a 250 ms timeout, and ACMD41 is limited to 1,000 idle responses; the overall
elapsed limit depends on task cadence. Legacy SDSC cards are rejected.

The caller owns power settling, SPI configuration, bus exclusion and any data
rate changes after success. Initialization does not determine capacity: read
CMD9 and decode its CSD with `sd::card()` before constructing `sd::Transport`.
The example retains that step, CID reporting, repeated sector comparisons and
partition inspection. No initialization operation writes card sectors.

Extraction validation: ASan/UBSan passed 33/33, formatting and cppcheck passed,
and the H563 A/B images built. Host tests cover delayed completion, restart,
command framing, response offsets, idle retry exhaustion, invalid voltage
echo/OCR, immediate rejection and failure at every transfer. H563 read-only
HIL passed both cases: five probes, CRC-checked repeated sector reads,
filesystem mount/list/missing-file handling and remount. Watchdog and retained
fault checks passed, with zero heap attempts and 3,576 observed stack bytes
(not a worst-case bound). No card or OTP writes; H755 was not flashed.

## Opt-in file creation

```text
fs unmount
fs mount rw
fs create "new-file.txt" "Hello from DaveOS"
fs unmount
fs mount
fs read "new-file.txt"
```

`fs mount` and `fs mount ro` mount read-only. Changing mode requires unmounting
first. The C++ API is `Volume::mount(bool writable = false)`; write mounts
require injected `write` and `sync` callbacks. Read-only protection is enforced
both in the wrapper and the FatFs disk bridge, even with a write-capable device.

`Volume::create(path)` uses `FA_CREATE_NEW`: existing files are never truncated.
`write(bytes, count)` reports the actual count (including short writes on full
media); `sync()` and `close()` report errors. The console command copies up to
128 bytes of text, creates a new file, writes, syncs and closes before logging
success. It adds no newline. Failure may leave an empty or partial new file;
creation is not an atomic transaction, and there is no automatic deletion,
overwrite, retry or power-loss guarantee. This phase does not expose append,
rename or formatting commands. With no RTC, FatFs uses the fixed date
in `storage/config.h`.

The SD writer sends CMD24, checks R1 before sending the token/data/CRC16,
checks the data-response token, and polls eight-byte ready windows with CS held.
The final byte must be 0xff; earlier bytes can straddle busy release. Polling
stops immediately on readiness, then requires clean CMD13 status. The original
one-second HAL deadline bounds the whole command/data/poll transaction and is
never restarted by polling. No fixed programming delay remains. Failures
invalidate the example's media readiness; unmount/probe/remount before retrying.
The caller still busy-waits through yield: this improves latency, not sleeping
power consumption. Other tasks can run during the wait.

Protocol reference: [Elm-Chan's SD SPI write sequence](https://elm-chan.org/docs/mmc/mmc_e.html).
The SPI response-check action inspects already received bytes while CS remains
asserted and aborts later actions on mismatch.

## Removing files

```text
fs unmount
fs mount rw
fs rm "some directory/file.txt"
```

`fs rm` removes one file, including its long name and allocated clusters; it
does not accept directories or perform recursive deletion. Read-only mounts
return `write_protected`, missing files return `no_file`, and directories
return `denied` (the root path may return `invalid_name`). The C++ operation is
`Volume::remove(path)`. Open file/directory handles and nested operations are
rejected as `locked`.

The worker owns a copy of the path and reports completion asynchronously.
Lookup and unlink share one volume guard across yielded work. FatFs flushes
metadata and calls the device's sync hook before success. As with creation,
a write/sync failure can leave an operation partially applied; there is no undo
or power-loss guarantee.

## Firmware installation from SD

Enable `bootloader=true`, `spi_sd_probe=true`, and `fatfs=true`. Networking is
optional. The same `application.ota` package generated for TCP works from a
file, for either destination slot. Copy it to the SD card with a host reader,
safely eject the card, then reconnect it to the board.

```text
sd probe
fs mount
ota init "/application.ota"
ota status
# Wait for "OTA SD prepared" before installing.
ota install
# Wait for "OTA SD done: ok" before resetting.
board reset
```

`ota init` requires an existing read-only mount. If mounted read-write, first
`fs unmount`, then `fs mount`. Preparation validates the header, complete block
structure, exact file length and reconstructed destination-image CRC without
changing flash. It reports version, product/layout, image/package sizes and
slot, and keeps the file reserved indefinitely. It does not require or imply
`ota enable`, which still gates TCP uploads.

A second init, competing filesystem commands and TCP uploads return busy while
reserved. Conversely, an active TCP update prevents preparation. `ota disable`
abandons the prepared/active update and disables network uploads. Cleanup runs
in the worker after pending I/O releases its buffers. Success, failure and
abandonment release the reservation; failed attempts require a fresh init.
File reads and close finish before the last payload is submitted for flash
verification/commit. The volume remains reserved until installation ends.
A disable arriving after the durable commit cannot undo the completed install.

`ota install` rereads the header, streams bounded chunks when the engine is
ready, verifies the installed CRC from flash, and commits a pending trial image.
It never resets automatically. The current application continues to run, and
normal A/B trial confirmation/rollback rules apply after `board reset`.
`ota status` retains the SD path, phase, progress and result after cleanup.
The source file is never modified or removed. Media-removal recovery is still
subject to the underlying SD driver's qualification limits.

`update/file.h` exposes the reusable worker and `storage/read_file.h` its
borrowed I/O contract. Source operations may yield but must return only after
releasing borrowed I/O buffers. `storage::Module::read_file()` supplies the
FatFs adapter and excludes both queued and executing filesystem commands.
The optional `update::Module<Event, true>` adds the command handlers and a
one-shot file worker; flash-engine polling remains periodic. Keep all owners
stationary and alive through cleanup. Host tests exercise real file-backed
FatFs and FileFlash, malformed packages, partial reads, reservation conflicts,
read/seek/close errors, cancellation and protocol exclusion.

Hardware test (explicitly opts into replacing the inactive firmware slot,
without writing SD files or OTP):

```sh
DAVEOS_HIL_SD_UPDATE_PATH=/application.ota build/hil-venv/bin/python -B -m pytest \
  --hil --hil-config build/hil-spi-h755.toml \
  tests/hil/h755/test_sd.py -k sd_update_opt_in
```

Copy a compatible paired A/B package first; it may be an older build. The case
starts from a factory image, reads the supplied package through the text file
preview command, independently reconstructs both images and compares installed
flash byte-for-byte. It checks that preparation leaves flash unchanged and
installs/boots/confirms A→B→A with concurrent console timers. H563 uses the shared case in
`tests/hil/h563/test_spi.py`, with its own build/package/config.

## Validation

SD-file OTA passed an H755 A→B→A HIL run using a compatible older package.
Preparation preserved the complete main-flash hash. Both installed images
matched independent reconstruction of the card's package, and both trial
boots confirmed. Read-only mount enforcement, reservation cleanup after a
missing file or disable, and exclusion of TCP updates also passed. Installs
took 21.99 and 21.80 seconds with 692 concurrent console timers; the maximum
host-observed timer round-trip was 26.25 ms. No SD sectors or OTP were written.
H563 SD-file OTA validation is recorded below. Physical power cuts and card
removal during preparation/install remain unqualified.


Host tests create a file-backed FAT16 image with a fragmented file, long name and
subdirectory. They exercise reads, seeks, EOF, missing files, malformed media,
registry exhaustion, media leases and nested-access rejection. Scripted SPI
tests cover CRC failure and timeout cleanup before releasing buffers.

The optional H563 `test_read_only_filesystem` HIL case factory-programs main
flash, probes the card, mounts, lists, checks errors and watchdog health, then
unmounts/remounts. It never writes card sectors or real OTP. File contents on the
DUT depend on the supplied card; synthetic host tests provide repeatable
file-content coverage.

H563 hardware validation passed both SD HIL cases: five repeated inspections,
then read-only mount/list, missing-file handling, rejection of a probe while
mounted, unmount/remount, and healthy watchdog/no retained fault. The supplied
card's root directory was empty during that initial run. No card writes were
performed. The measured stack watermark after
filesystem operations was 2,936 bytes out of 485,080 available, with zero heap
requests; this is workload evidence, not a worst-case bound.

The host suite passed 32/32. ASan/UBSan passed its full 32-test suite and the
updated storage cases; logging-disabled storage tests and TSan storage/yield
tests passed too. Formatting and cppcheck passed. H563 A/B firmware was built
and tested; H755 was not programmed for this integration.

A subsequent read-only check on the user's populated card passed 69 file-read
requests across six files (2,908–21,154,218 bytes), including both subdirectories
and a long filename. Repeated/overlapping reads agreed across 512-byte sector
boundaries and 32 KiB cluster boundaries at offsets 32,768 and 65,536. A maximum
4,096-byte preview spanning a cluster boundary matched two separate 2,048-byte
reads. Every file passed short-read-at-EOF, exact-EOF, and past-EOF rejection
checks. Watchdog health and the retained-fault check passed afterward.

These checks establish read/seek consistency, not byte-for-byte comparison
against independently obtained originals or qualification of fragmented files
on hardware (fragmented-file coverage remains in the host tests). No card data
was written. Card-removal recovery and large-directory workloads remain open.

The opt-in write HIL case subsequently created
`daveos-write-test-20260921-c.txt`, synced/closed it, refused a second creation
of the same name, remounted read-only and verified all 63 bytes. Read-only
mounting rejected creation as expected. Watchdog/fault checks passed; the stack
watermark was 3,488 bytes with zero heap requests. Two initial attempts left
empty `daveos-write-test-20260921.txt` and `daveos-write-test-20260921-b.txt`
files when the first busy-release byte was 0x03. The writer now clocks an
eight-byte ready window and checks its final byte; the partial-byte case is
covered by a scripted regression test. The empty files were left intact.

Host write tests cover remount/readback across clusters, preservation of an
existing file, create-only semantics, empty files, disk-full short writes,
write/sync failures, read-only enforcement and copied command arguments.
Scripted SPI tests reject command/data/status errors and a still-busy card.
Write HIL is opt-in through `DAVEOS_HIL_SD_WRITE_PATH`; ordinary HIL runs do not
create files. The current opt-in case also removes its newly created file,
remounts and verifies absence. It never selects an existing file for deletion.
This is initial write validation, not power-loss qualification.

After creation, all 69 original-file checks passed again and the saved
pre-write content-sample hashes matched. Full ASan/UBSan passed 32/32; targeted
HAL/storage tests passed under TSan and with logging disabled. Formatting and
cppcheck passed. H563 A/B firmware was built/programmed for these checks;
H755 remains build-only for the bus changes.

Removal validation passed in ASan/UBSan and logging-disabled storage tests:
subdirectory/long-name removal, persistence across remount, preservation of
another file, rejection of directories/root/read-only/open-handle operations,
missing/invalid paths, sync-error propagation and borrowed command-path copying.
On H563 the opt-in cycle created and removed `daveos-rm-test-20260921.txt`,
then remounted and confirmed `no_file`. Read-only and root-path rejection,
watchdog health and retained-fault checks also passed. Only that newly created
test file was removed; previous files remain. Formatting and lint passed.

Bounded busy polling passed the H563 opt-in create/readback/remove/remount
cycle. Against the preceding fixed-delay run on this card, logged command
completion times changed from 4,079 to 1,082 ms for creation and 2,543 to 546 ms
for removal. These are observed end-to-end runs, not general throughput bounds;
the then-current fixed-window read path remained a significant cost. The temporary test file
was removed. ASan/UBSan passed 32/32, targeted TSan HAL/storage tests passed,
both H563/H755 A/B builds passed, and formatting/lint passed. H755 was not
flashed for this change.

H755 SD bring-up uses PA5/PA6/PB5 and PD14 GPIO CS. Both read-only HIL cases
passed: five initializations and 60 CRC-checked sector reads at 250 kHz/1 MHz,
FAT32 identification, mount/list/error handling and remount. Stack painting
observed 3,480 of 44,072 bytes, with zero heap attempts and healthy watchdog/
fault checks. The image moves 44,544 bytes of fixed lwIP pools into AXI SRAM
to preserve DTCM stack headroom. This phase did not write card sectors or OTP;
The subsequent H755 write test is described below; card removal and higher
SPI rates remain unqualified.

H755 UART/USB/TCP command, CRC, large-packet ping, TCP reconnect and software
reset regression passed 4/4 after the memory-placement change. ASan/UBSan
passed 33/33; both H563/H755 A/B builds, the H755 standalone build, formatting
and cppcheck passed. The SD HIL cases are shared in `tests/hil/sd_cases.py`.
The card now resides on H755, so H563 received build coverage only in this
bring-up phase; its earlier SD hardware results above remain historical.

The H755 opt-in write HIL case subsequently passed: it created
`daveos-h755-20260921-203034.txt`, synced/closed it, verified the exact contents
after remount, and removed that same file. It verified overwrite refusal,
read-only mount protections, root-directory removal rejection and absence
after a final remount. Create took about 1.09 s and removal 0.55 s in this run;
these are observed command durations, not throughput bounds. Stack painting
observed 4,064 of 44,072 bytes, with zero heap attempts, a running watchdog
and no retained failure. Only the test file was created/removed; OTP was not
written. This was one write cycle, not endurance or power-loss qualification.

The incremental-read / H755 DMA follow-up passed all three SD HIL cases:
five probes (60 CRC-checked sector reads), read-only filesystem checks, and a
new-file create/sync/close/remount/exact-readback/remove cycle. Only
`daveos-dma-20260921-205112.txt` was created/removed. Creation took 93 ms and
removal 50 ms, versus approximately 1.09 s / 0.55 s before this change on the
same card. Most of this improvement removes unnecessary read clocks; it is
not a DMA-only throughput comparison.

With identical incremental reads at the same SPI rates, five probes produced
45,915 SPI IRQ entries without DMA and 4,103 combined SPI/DMA IRQ entries with
DMA (about 91% fewer). The DMA run moved 30,720 bytes in 60 chunks. Probe times
were essentially unchanged, around 188–189 ms each: wire time and task cadence
still dominate. Backend polls dropped from 45,665 to 3,853; these counts do not
measure CPU utilization. `-Dspi_sd_dma=false` reproduced the interrupt-only
results. The default H755 build was restored to DMA afterward.

ASan/UBSan passed 33/33, targeted TSan HAL/storage/SD tests passed 3/3,
H563/H755 A/B firmware built, and format/lint passed. H563 was not reflashed in that phase; see the H563 follow-up below.
Interrupted-block/card-removal
recovery, injected DMA timeout/error cleanup, cache-enabled operation and higher SPI rates remain
unqualified. During DMA bring-up, a rejected transfer left this card mid-block;
repeated probes restored it, but immediate recovery after that failure is not
guaranteed. No OTP writes were performed.

The restored DMA build also passed H755 UART/USB/TCP commands, CRC,
1,400-byte ping, TCP reconnect and software-reset regression (4/4 HIL).

A read-only concurrency check then read 4 KiB from `enum_builder.h` ten times
while another TCP connection repeatedly requested 1 ms board timers. All 250
timer commands completed; maximum host-observed round trip was 8.4 ms,
including console/network delivery (not a scheduler deadline bound). Watchdog
and retained-fault checks passed. The filesystem was unmounted afterward.

### H563 DMA follow-up

H563 now uses GPDMA1 channels 1/2 for SPI1 RX/TX with private SRAM staging.
Read-only SD/filesystem HIL passed, including five probes and 60 CRC-checked
sector reads: 30,720 bytes in 60 DMA chunks, with 7,113 combined SPI/DMA IRQ
entries. Each probe took about 187 ms at the existing 1 MHz rate. These are
observed runs, not throughput guarantees or a direct comparison with H755.
Temporary-file create/readback/remove passed, as did rejection of the H755
package during `ota init`; the failed preparation released the filesystem.
Observed stack peaks were 3,568 bytes for reads and 4,016 for writes, from
490,224 bytes available, with zero heap attempts. Only the uniquely named
HIL test file was created and removed; real OTP was untouched.

H563 UART/USB/TCP commands, network/reconnect, reset/USB reconnect and version
checks passed 6/6. ASan/UBSan passed 33/33, portable Python passed 21 with 5
opt-in skips, H563/H755 A/B builds passed, and the H563 IRQ-only SD updater
build without networking also passed. Formatting and lint passed.

H563 SD-file OTA also passed A→B→A from `/app_h563.ota`: unchanged full-flash
snapshots during preparation, byte-exact installed images, competing TCP
request rejection, cleanup after failed/abandoned preparation, and both
explicit resets followed by successful trial boots and health confirmation.
Installs took 37.31/37.34 seconds; 1,184 concurrent TCP timer commands completed
with a maximum 24.49 ms host-observed round-trip. Stack painting after the
first install showed 3,912 of 490,224 bytes used and zero heap attempts.
The board was left running confirmed A, with the filesystem unmounted.

Two preliminary runs stopped before installation: CMD0 read only 0xff after
card handling (reseating the wiring restored five complete inspections),
and the test's single 2 MiB flash read exceeded its host-side timeout. The
shared HIL case now reads flash in bounded 256 KiB pieces while retaining the
complete comparison. No update-related SD or OTP writes were made. Physical
power cuts, card removal and injected DMA abort/error recovery remain outside
this validation.

Pre-push regression passed host 32/32, ASan/UBSan 33/33, TSan 32/32, fake
26/26, logging-disabled 34/34, and portable Python 21 passed with 5 opt-in
skips. Both SD A/B firmware builds and programming plans, formatting and lint
passed. These software checks supplement the hardware runs above; the user
also manually completed the H563 SD update workflow successfully.
