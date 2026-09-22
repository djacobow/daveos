# Architecture-refactor hardware validation

Validated commit `ac56ba76e2bd3f505ab5aedae71721ea451c44c8` on 2026-09-22.
Both Nucleo boards had ST-LINK, USB device and Ethernet connected. H563 also
had the SPI SD card and MCP3425 on I2C1 PB8/PB9; H755 had no SD card.
Each HIL case started from a factory main-flash image.

## Results

| Configuration | Result |
| --- | --- |
| H563 full suite | 31 passed, 8 skipped, 1 host-harness failure |
| H563 memory reruns with Watcher fix | Passed twice |
| H563 temporary-file create/read/remount/remove | Passed |
| H563 foreign-board SD package rejection | Passed |
| H563 flash-emulated OTP suite | 4 passed |
| H563 SD DMA timeout cleanup | Passed |
| H755 non-SD suite | 24 passed, 6 SD cases skipped |

After the isolated reruns and opt-in cases, 39 distinct H563 cases and 24 H755
cases passed. H563 SD OTA is the only case in its full suite not exercised.

The H563 file-test invocation initially included an unsupported leading slash;
the harness rejected it before any card operation. The corrected invocation
passed, including create-only protection and removal of the temporary file.

Coverage includes UART/USB/TCP commands, timers, CRC, Ethernet, TCP OTA in both
directions, journal reclamation, trial rollback, corrupted-image fallback,
no-valid-image recovery, watchdog failures, retained fault capture and memory
checks. H563 additionally passed ADC sampling, I2C scanning/reset recovery,
read-only real OTP checks, repeated SD reads and filesystem operations.
The flash emulator passed exhaustion, duplicate-write handling, torn-lock
recovery, preservation across A/B updates, and writes during OTA in both banks.
Each board's UART stress run received all 1,160 expected replies without
reported drops or transmit errors.

Fresh H563 and H755 A/B ARM builds passed, as did the H563 OTP-emulator build,
format check and lint. This run did not rerun the complete host C++ suite.

## Harness fixes

H563's original memory case failed while closing a USB serial stream:
Watcher's reader thread and its owner could both call `Serial.close()`.
The resulting `TypeError` was a host resource-close race, not a target memory
failure. The local Watcher checkout now serializes closure and performs it
once. Its test suite passed (37 passed, 2 skipped), including a deterministic
concurrent-close regression test. Both H563 memory reruns passed using that
checkout through `PYTHONPATH`.

The Watcher fix was published as commit `5e9dfb3` on its `main` branch.
DaveOS's `requirements-hil.txt` now pins that published commit.

The SD DMA fault-injection test's debugger expression was updated from
`sd.backend_` to `sd.hardware_.backend_` to follow the extracted SD hardware
component.
The corrected timeout test passed: DMA channels and requests stopped, CS was
released, filesystem access was rejected, and no automatic retry occurred.
An explicit controller reset succeeded, but the interrupted card then rejected
CMD0. Its adapter needs a 3V3 power cycle before further SD use. H563 was left
with the full-feature factory firmware; H755 retains its non-SD factory build.

## Memory observations

| Workload | Stack region | Observed watermark | Heap attempts |
| --- | ---: | ---: | ---: |
| H563 commands | 488,816 B | 3,452 B | 0 |
| H563 filesystem write | 488,816 B | 3,976 B | 0 |
| H755 commands | 61,176 B | 2,904 B | 0 |

Stack paint measures overwritten bytes in these workloads, not a worst-case
stack requirement or every possible stack-pointer excursion.

## Scope and artifacts

SD OTA was explicitly skipped at the user's request. H755 SD testing was
excluded because the card was on H563. No real OTP writes or locks were
performed. Physical power interruption, card removal and electrical I2C/SPI
qualification remain outside this run.

Local artifacts are under `build/`: `review-h563.xml`,
`review-h563-extra.xml`, `review-h563-repeat.xml`, `review-h755.xml`,
`review-emulator.xml`, `review-h563-timeout.xml`, their
corresponding logs, and saved `review-*-manifest.json` files. Individual UART,
debugger and memory evidence lives under `build/hil/`. Those directories are
reused across runs; old files and appended timing logs are not evidence of
additional tests in this run.
