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

## H563 SD example

Use the [SPI A wiring](spi-i2c.md) with GPIO-controlled CS:

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
It reuses the probe's fixed capture window at 1 MHz, including clocks for a
100 ms response allowance even when the card answers early. This prioritizes
reuse and validation over throughput; staged response capture, multiblock reads
and SPI DMA remain future work. Only SDHC/SDXC cards are supported.
On a read failure, unmount and run `sd probe` again before remounting.
There is no hot-plug automount or card-detect pin contract.

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
checks the data-response token, and holds CS through a conservative 500 ms
programming allowance. It then requires ready and clean CMD13 status. Each
sector has a one-second HAL deadline; failures invalidate the example's media
readiness. The next operation requires unmount/probe/remount. The fixed allowance
is deliberately slow; adaptive busy polling is future work. Other tasks can
run through yield during the wait.

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

## Validation

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
