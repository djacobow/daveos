# H563/H755 validation after instance naming and CI changes

Tested working tree based on `cca30ac`, including instance-name propagation,
starter pin updates, expanded CI builds, and the listener-capacity fix below.
This is hardware evidence for these configurations, not full qualification.

## Wiring and configuration

- H563 ST-LINK `0043001A3433510E37363934`: SD on SPI1 (PA5 SCK,
  PG9 MISO, PB5 MOSI, PD14 GPIO CS); MCP3425 on PB8/PB9 I2C1.
- H755 ST-LINK `002E002D3234510233353533`: SSD1306 128×64 on PB8/PB9
  I2C1, without reset GPIO; no SD card attached.
- Both boards: ST-LINK UART, USB device, Ethernet/DHCP, A/B boot firmware.
- H563 features: UART, USB, health, networking, TCP console, SD/DMA, FatFs,
  file transfer, OTA, ADC, and read-only real OTP.
- H755 features: UART, USB, health, networking, TCP console, SD/DMA, FatFs,
  file transfer, OTA, display. SD-dependent tests are excluded without a card.
- Each pytest hardware case starts from a mass-erased, verified factory image.
  Real OTP is not erased or programmed. `otp_programming=false` throughout.

## Issue found and fixed

The first H563 run failed the two initial OTA cases with connection refused.
UART/USB and DHCP worked. The fixed lwIP pool allowed only two listening PCBs,
which the console and file service consumed before OTA was enabled. Increasing
`MEMP_NUM_TCP_PCB_LISTEN` to three permits ports 1000, 1001 and 1002 to coexist.
A real-packet host regression establishes three simultaneous connections.
The corrected hardware run passes the previously failing journal/OTA cases.

A second issue was in test selection: `pytest_ignore_collect` returned `False`
for ordinary paths. Because it is a first-result hook, that prevented pytest's
own `--ignore` handling. It now returns `None` unless explicitly excluding HIL.
A portable subprocess regression checks that explicit HIL file exclusions work.
The H755 run was stopped and restarted with the intended 25 cases; eight cases
had passed before that restart, but are not counted twice.

## Results

- H563 main suite: **34 passed, 7 skipped**. Includes console/network/reset,
  journal rollover and invalid-image recovery, retained faults, file transfer,
  MCP3425 conversions and interrupted-read recovery, memory checks, TCP OTA
  A→B→A and interrupted upload recovery, read-only OTP, SD/FatFs reads and
  temporary-file create/readback/remove, UART bursts, and watchdog recovery.
- H755 selected suite: **25/25 passed**. Console/network/reset, SSD1306
  automatic/manual updates, memory, basic/floating-point and other retained
  faults, watchdog/startup failures, debugger pause, UART bursts, TCP A→B→A,
  slot-B factory boot, journal reclamation from both slots, trial rollback,
  corrupt-image fallback, no-valid-image recovery, and bootloader watchdog.
  SD/file-service tests were excluded because this board has no card attached.
- Observed stack paint: H563 file transfer 4,216 bytes; H755 command workload
  3,488 bytes. Both recorded zero heap attempts. Paint is not a worst-case bound.
- Host ASan/UBSan after the listener fix: **45/45 passed**, including the test-selection regression registered in Meson; formatting and lint
  passed. Both ARM configurations rebuilt successfully. Portable Python checks
  passed 39 tests, with seven opt-in skips. The pre-push repeat also passed
  fake/logging-disabled 39/39, TSan 44/44, and host logging-disabled 46/46
  (including the HEAD starter consumer).

The seven initial skips are four flash-emulator cases, the explicitly deferred
SD DMA timeout injection, and two SD package cases reserved for a separate run.
Separate follow-ups passed:

- Bank-B OTP emulator: **4/4**, including reset/factory behavior, A→B→A
  persistence, torn-lock recovery, and writes during OTA in both banks.
- SD OTA: **2/2**, using newly uploaded packages. H563 A→B→A verified the
  flashed bytes, unchanged flash during preparation, trial boots, confirmation,
  and concurrent TCP timers. The H755 package was rejected on H563.
- Pinned hardware starters at `cca30ac`: H563 and H755 device starters passed
  periodic worker, help, timer and reset; H563 storage starter passed card
  startup, read-only mount/list/unmount, help, timer and reset. H755 storage
  starter remains build/programming-plan tested only without an attached card.

That is **65 passing pytest HIL cases plus three hardware starter smoke checks**.
The initial interrupted/failed runs are retained in the logs, not counted as
additional passes. The explicitly deferred DMA injection is the only H563 main
suite case not subsequently run.

Both boards were factory-restored afterward and reported a running watchdog,
confirmed image and no retained failure. H563 SD and ADC were checked again;
H755 live display was enabled. Temporary `/naming-h563.ota` and
`/naming-h755.ota` files were removed and the H563 filesystem unmounted.
At completion, DHCP addresses were H563 `192.168.1.207` and H755 `192.168.1.147`;
these are observations, not fixed configuration.

## Boundaries

SD DMA timeout injection was explicitly deferred by the user because it may
require physically power-cycling the card. No physical power-cut campaign,
card removal, calibrated ADC measurement, or new visual OLED confirmation is
part of this run. A passing display test demonstrates I2C writes and automatic
update/pause behavior, not inspection of the pixels. H755 filesystem hardware
coverage remains unavailable while its card is on H563.

## Local artifacts

Logs and JUnit reports are in `build/naming-hil-*`; per-board manifests are
`build/naming-hil-h563-manifest.json` and `build/naming-hil-h755-manifest.json`.
Detailed UART, GDB and OpenOCD transcripts remain under `build/hil/`.
