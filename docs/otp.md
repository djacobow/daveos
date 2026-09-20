# OTP storage and validation

Backends include a persistent host model, a bank-B flash emulator, and real
H563 OTP. The real adapter defaults to read-only. One explicitly authorized
serial write and permanent lock have been validated on the H563 DUT.
The complete hardware contract and qualification sequence are in
[PROJECT.md](../PROJECT.md#otp-storage-agreed-behavior-and-implementation-design).

`daveos::otp::Store` uses an injected synchronous `otp::Driver`. It scans 32
64-byte records into a fixed RAM cache during `init()`. Read access never touches
the backend. Constructors do no I/O, and all operations belong on the scheduler
thread. The driver and any injected CRC backend must outlive the store.

The only supported record type is a serial number: 1–56 printable ASCII bytes,
including spaces. Each changed value appends a record, verifies readback, then
locks that row. Identical values do not consume another row. A lock failure
returns an error but leaves the verified new value readable; submitting that
value again retries only its lock. Failed writes can consume rows. There is no
erase API.

## Composition

Link the Meson dependency `daveos-file-otp` for the host model, or `daveos-otp`
for the store and optional module with another driver. Within this repository,
the corresponding dependency variables are `file_otp_dep` and `otp_dep`.

```cpp
#include "otp/module.hpp"
#include "platform/host/otp.h"

namespace otp = daveos::otp;
namespace host = daveos::platform::host;

// The path and objects must outlive their borrowers. Create mode is exclusive:
// use OpenMode::existing to reopen this file on subsequent runs.
host::FileOtp backend("build/device.otp", host::FileOtp::OpenMode::create);
otp::Store store(backend.driver());
otp::Module<> provisioning(store);
```

Add `&provisioning` to the application's `ModuleList`. It scans during stage1;
other modules can read `store.serial()` in stage2. Without a scheduler, call
`store.init()` directly. `serial()` returns an optional borrowed string view;
a missing serial is valid empty storage, whereas `ready() == false` means the
store is unavailable. A view lasts until the next verified value change.

The module registers:

```text
otp serial
otp serial set "ABC 123" "ABC 123"
otp status
```

Both setter strings must match exactly. Quoting and escaping use the existing
command parser. The store works without logging or command dispatch; the module
still accepts commands in logging-disabled builds, with output elided.

## Persistent failure tests

```sh
meson test -C build/host otp --print-errorlogs
```

`FileOtp` opens only during initialization and exclusively locks its backing
file. Existing mode rejects missing or malformed files; create mode never
truncates an existing file. Files have a versioned 64-byte header and 32
128-byte physical images. Images track logical bytes, each halfword's write
state, and the permanent lock separately. Programmed `0xFFFF` data is consumed.

`fail_after(n)` injects a one-shot error after `n` durable mutation steps; zero
fails before any mutation. Each halfword persists intent, data, then completion;
the record's type halfword is last. The next step locks the row. Reopening a
file preserves every interrupted attempt. `inject_read_error()` models a
persistent per-halfword ECC failure. These APIs are fixture tools, not proof of
physical OTP behavior under power interruption.

`Store::snapshot()` reports used/appendable slots, anomalous earlier gaps,
invalid/unknown/unreadable records, the current serial's block and lock state,
and the last error's phase, block and status. An earlier gap is never reused.

## H563 bank-B emulator

Enable it explicitly on an H563 A/B build:

```sh
meson configure build/boot-h563 -Dotp_backend=flash-emulator
meson compile -C build/boot-h563
build/hil-venv/bin/python -B -m pytest tests/hil/h563/test_otp.py \
  --hil --hil-config build/hil.toml -v
```

The default is `-Dotp_backend=none`. The emulator option rejects builds without
`-Dbootloader=true` or with another board. Meson includes a separate composition
file; no conditional feature macros enter the application. HIL always installs
a factory image first and clears emulated records. Real OTP remains untouched.

`otp::FlashOtp` borrows a `boot::Flash` driver and a reserved sector base address.
The generated H563 layout exports `boot::kOtpEmulatorBase` (`0x08100000`) and
`boot::kOtpEmulatorSize` (8192 bytes). Metadata B remains at `0x08108000` and
application B at `0x0810A000`.

Each 256-byte physical slot contains a claim, a 64-byte record body, a commit,
and ten lock-attempt cells. Torn rows are consumed; a torn lock can retry in a
fresh cell of the same row. Valid records survive resets and A/B OTA. Only
factory main-flash mass erase clears the emulator; full storage never triggers
an erase. FileFlash tests enforce physical write-once addresses and inject torn
writes at every byte of every record phase.

Writes synchronously poll only operations accepted by their own driver. Each
flash-word wait is bounded to 100 ms and a finite poll count. A timeout disables
that emulator instance until reset and preserves its pending write buffer.
STM32 flash adapters retain shared peripheral ownership until the initiating
instance polls completion, even when hardware finishes earlier. Concurrent OTA
may cause an OTP request to return `busy`; the application can retry later.

Emulation establishes the record/service behavior, not the OTP fuse, MPU,
lock-option-byte, ECC/NMI, or physical power-cut behavior of the eventual real
OTP adapter.

Validation on NUCLEO-H563ZI covers serial commands on UART/USB/TCP, all 32 rows,
identical/full writes, confirmation rejection, reset retention, factory clearing,
torn-lock retry, byte-for-byte retention through A→B→A OTA, and serial writes
during both update directions. A busy request preserved the prior value while
OTA continued. Adjacent placeholder/metadata bytes remained unchanged. H755 OTP
and physical power-cut qualification are outside this validation.

## Real H563 OTP: read-only first

Select `-Dotp_backend=h563 -Dotp_programming=false` on an H563 boot-layout build.
The default programming setting is false. It prevents both data programming and
permanent lock changes at the driver boundary. All automated HIL runs reject a
build with real programming enabled, and the real-OTP test sends only read and
reset commands:

```sh
build/hil-venv/bin/python -B -m pytest tests/hil/h563/test_otp_hardware.py \
  --hil --hil-config build/hil-otp.toml -v
```

Configure that local TOML to name the real-OTP build. As with other HIL tests,
factory provisioning replaces main flash but does not erase physical OTP.
Commands `otp status` and `otp serial` use the RAM cache. `otp_hw inspect <block>`
performs a guarded read and reports halfword counts, lock state and a fingerprint;
it does not expose unknown payload contents or write anything.

The H563 adapter checks the board's existing noncacheable/XN MPU mapping and
uses volatile 16-bit reads. Its NMI guard matches the OTP region and reported
address group, captures ECCDR before acknowledgement, and stays active through
the delayed exception. Programmed `0xFFFF` with no ECC indication is consumed;
only matching unwritten-cell indications classify a halfword as virgin. See
[ST's description of unwritten OTP reads](https://community.st.com/stm32-mcus-60/handling-ecc-errors-in-stm32h5-series-reading-unwritten-otp-and-flash-data-area-143933).

This DUT's read-only qualification found block 0 fully programmed and locked,
with contents not recognized by the new record format. Blocks 1–31 showed only
virgin halfwords, no locks and no read errors. Repeated scans and reset preserved
all fingerprints and locks. The subsequent explicitly authorized provisioning
stored `dave_nucleoh563_sn001` in block 1 at `0x08FFF040` and verified its
permanent lock. Block 0 and blocks 2–31 retained their fingerprints and locks.
The read-only firmware was restored; factory programming and reset preserved
the serial and all OTP fingerprints/locks (read-only HIL passed).

The hardware-validated provisioning path programs each
halfword once (type last), temporarily changes only the selected MPU region's
access permission, restores it, shares controller ownership with OTA/boot, and
checks effective OTPBLR_CUR locks without implicit reset. Further automated
hardware testing must use the read-only build and must not write or lock OTP.
Additional real writes require explicit user instruction. Physical power-cut
qualification remains deferred.
