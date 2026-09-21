# Follow-up work

Completed validation entries describe their respective phases. The H755 parity
section supersedes older H755 hardware deferrals; remaining gaps are explicit.

## Bootloader and reliability

### Completed

- [x] H563 bootloader with fixed 32 KiB reservation, redundant metadata, equal 984 KiB A/B slots, CRC verification, one-trial policy, and explicit durable/idempotent confirmation.
- [x] Factory HEX/programming targets: erase main flash, preserve OTP, install confirmed A. Compile STM32 sources with `-Os` and debug information.
- [x] Link the same objects for A/B and build a paired relocation package; validate exact reconstruction and execute both slots on H563.
- [x] Injected software/hardware CRC and flash drivers; invalidate H563 ICACHE after flash mutations before readback.
- [x] Cooperative TCP OTA on port 1001, application enable/disable/status, Python uploader and optional reboot. Binary updates remain separate from text consoles.
- [x] H563 validation: trial rejection, confirmation, corrupted-confirmed-image fallback, repeated A/B resets, and four TCP uploads in both directions. Uploads took about 35.5 s with 670 concurrent console timer checks (maximum 13 ms response).
- [x] Host tests for journal power-loss boundaries, fragmented uploads, CRC rejection, disconnect/abort paths, failed-final-commit recovery, and persisted file-backed OTA/boot/confirmation.
- [x] FileFlash for host/fake simulations: raw persistent files, erase/program rules, injected clock, close/reopen tests. Does not model hardware latency or STM32 ECC.
- [x] Watchdog controller, injected IWDG driver, scheduler-progress snapshots, and retained-fault record foundations; integration work follows below.

### Startup and runtime health

- [x] Start H563 IWDG before clock/peripheral/module initialization; reset on explicit init failure and let startup hangs expire.
- [x] Feed only after heartbeat and every active repeating task's progress checks; latch failures and preserve their diagnostics across reset.
- [x] Automatically confirm a trial after five seconds of healthy scheduling; USB/Ethernet/DHCP availability must not gate confirmation.
- [x] Validate H563 debugger freeze, latched task-rate failure, an interrupt-enabled runtime hang, early-warning exception-frame/CRC capture, natural watchdog reset, retained reporting, and automatic trial confirmation.
- [x] Stall H563 trial B in SystemClock_Config before clock setup: verify IWDG early-warning frame/CRC, natural reset, rejection of unconfirmed B, and healthy confirmed-A fallback.
- [x] Add H563 interrupt-masked-hang assertions (reset without a new frame), valid PSP, inaccessible PSP, stack-limit faults, and fault recovery with core debugging disabled. ST-LINK stays physically connected. Fix frame validation to reject CFSR.STKOF; hardware verifies invalid frames are not copied.
- [x] Report retained watchdog/init failures on startup and provide health status/fault/clear commands.
- [x] Integrate H563 application HardFault/BusFault/MemManage/UsageFault handlers with retained capture; trigger all four CPU faults on hardware and verify type/status bits, frame PC, CRC, reset and next-boot reporting. Add tests/hil/h563/test_faults.py for repeatable injection. ASan 24/24, format/lint, H563 boot/standalone/starter builds pass.
- [x] IWDG validation: ASan 24/24, fake 18/18, formatting/lint, H563 boot/standalone and H755 builds; OTA with the watchdog active and automatic confirmation on H563. H755 hardware remains deferred.

### H755 reliability parity

- [x] Share the application health module through board-provided CRC/watchdog/fault adapters. Start M7 IWDG1 before clock/peripheral initialization; M4 continues to sleep. H755 has no IWDG early-warning IRQ, so arbitrary hangs reset without a captured frame.
- [x] Reserve 2 KiB of DTCM for retained faults and the exception stack. Hardware testing found a byte-written record lost a magic byte across reset; publish full 32-bit words with magic last. Basic and floating-point frames now survive reset and validate their CRC.
- [x] Exercise UART/USB/TCP commands and hardware/software/incremental CRC, DHCP/large-packet ping/TCP reconnect, software reset and USB recovery, all four CPU faults, latched task-progress failure, interrupt-masked/startup hangs, and explicit initialization failure on H755.
- [x] H755 reliability HIL: 15 cases passed across the main run and focused reruns, including a debugger pause longer than the IWDG timeout. The first UART stress run exposed bounded TX overflow; increase H755 ping-pong buffers to 8 KiB each (H563 stays at 4 KiB). Two repeated runs then delivered all 1,160 burst replies with zero dropped frames or DMA errors. No new visual LED/button, physical cable/power-cut, or standalone-starter hardware validation.
- [x] Parity regression: host 27/27, ASan/UBSan 27/27, fake 21/21, logging-disabled 28/28, TSan 26/26, GCC 13 28/28; portable Python 18 passed with 5 opt-in skips, H563/H755 standalone starter builds, formatting/lint, H563 OTP/boot firmware, H755 full/none/UART/USB configurations, and both programming plans passed. H563 was not reflashed; a read-only network query verified its existing OTP serial remained `dave_nucleoh563_sn001`.
- [x] Implement the H755 A/B layout: one 128 KiB sector each for the bootloader (32 KiB code limit) and fixed factory-programmed M4, one metadata sector per bank, and equal 768 KiB M7 slots. Factory HEX contains both cores; OTA preserves both fixed-image sectors. IWDG starts in the bootloader and continues through application handoff.
- [x] Add injected H7 flash operations with 32-byte programming, version-2 journal encoding, file-backed geometry tests and torn-write/checkpoint/erase simulations. Runtime reclamation checkpoints in the executing bank and erases only the inactive bank; checkpoint exhaustion returns `full` without resetting. Boot maintenance restores checkpoint space. Preserve H563's version-1 encoding and the 100 ms task-progress allowance.
- [x] Full H755 A/B HIL: 23/23 passed in one run. Covers UART/USB/TCP, CRC, network/reset recovery, retained CPU faults, startup/runtime/bootloader watchdog hangs, failed-trial rollback, corrupt-confirmed fallback, both-images-invalid reset/factory recovery, confirmed B startup, bidirectional OTA and runtime journal reclamation in both slots. Both fixed-image sectors retain their hashes through OTA. UART stress delivered 1,160/1,160 replies without drops or DMA errors. OTA had 207 concurrent timers with maximum 26 ms host round-trip; journal rollover had 265 timers with maxima 7 ms (A) and 5 ms (B). These are host-observed times, not direct scheduler latency measurements; task-health checks remained active with the unchanged 100 ms allowance. The final rebuilt firmware/package/factory bytes match those tested. Bootloader is 20,896 of its 32,768-byte code limit. Left H755 running confirmed A.
- [x] H755 A/B software regression: host 27/27, ASan/UBSan 27/27, fake 21/21, logging-disabled 28/28, TSan 26/26, GCC 13 28/28; portable Python 19 passed (5 opt-in skips), all three standalone starter builds, format/lint, H563 OTP/boot build, H755 standalone transport combinations, and both programming plans passed. A fresh H755 A/B build verified application/bootloader/package CI build-number propagation. H563 was not reflashed and real OTP was untouched.
- [ ] Qualify H755 physical power cuts, actual flash ECC injection, and cache-enabled flash operations. Runtime checkpoint exhaustion/recovery has host-model coverage; hardware currently exercises reclamation in both directions.
- [ ] Plan H755 OTP separately; its existing H563 backend/emulator must not be selected on H755.

### Remaining qualification and follow-ups

- [x] Generate independent bootloader/application stamps, print boot identity and expose board version. Local CI-equivalent host/ARM builds verified build 12345 and independent boot version 7.9; GitHub workflow now checks both stamps and the OTA package.
- [x] Verify version propagation on GitHub: run 35531195219 passed all four jobs, including application/bootloader/OTA build 26 identity checks.
- [x] Fix CI blockers: use an explicit constexpr intermediate for GCC 13 command macros in templated modules, and declare the host I/O helper as a Meson test dependency so test-only builds produce it.
- [x] H563 journal rollover/reclamation, both-images-invalid reset loop/factory recovery, and OTA timeout/disable/reset/PHY-link-loss/replacement tests. All 26 HIL cases passed across the main run and focused completion runs after fixing a GDB file-local-symbol lookup in the PHY test helper. Includes 1,160 UART burst replies without drops/errors, bidirectional OTA with concurrent timers, and startup trial rollback. Host tests also cover abort/disable during every flash phase, including an outstanding journal commit-marker write.
- [ ] Deferred by user: physical power-interruption qualification. Reset injection and host flash models do not replace power-cut tests.
- [x] Qualification matrix: host 26/26, ASan/UBSan 26/26, fake 20/20, logging-disabled 27/27, TSan 25/25, formatting/lint, H563/H755 firmware and both standalone starters pass. H755 bootloader integration and hardware testing remain deferred.
- [x] Specify append-only OTP records, RAM cache, serial validation/confirmation commands, permanent locking and lock-only retry. Record backend contracts, file format, bank-B emulator layout and phased tests in PROJECT.md.
- [x] Implement OTP Store/module and persistent host FileOtp with halfword/commit/lock failure injection and reopen tests. Includes confirmation commands, RAM-cache/lock-only behavior, corruption/unknown/ECC handling, and all 99 simulated interruption boundaries. Validation: 13 OTP cases / 1,620 assertions; full host 27/27, ASan/UBSan 27/27, fake 21/21, logging-disabled 28/28, TSan 26/26, GCC 13 28/28; formatting/lint and H563/H755 ARM builds passed. That phase used host models only; no hardware was flashed or tested and real OTP was untouched.
- [x] Implement the 8 KiB bank-B FlashOtp emulator, explicit Meson selection and shared H563 flash ownership. Validation: 18 OTP cases / 9,053 assertions, including 112 torn-write/reopen cases, lock exhaustion, ECC, busy and timeout paths; full host 27/27, ASan/UBSan 27/27, fake 21/21, logging-disabled 28/28, TSan 26/26, GCC 13 28/28. H563/H755 builds, H563 programming plan, formatting/lint passed. Seven selected H563 HIL checks passed: serial over UART/USB/TCP, deduplication/confirmation, reset retention, 32-row exhaustion, factory clearing, torn-lock retry, A/B OTA retention, writes during OTA, console/timer/watchdog-status smoke. Dumps verified adjacent reserved/metadata bytes unchanged; a busy OTA-time request preserved the prior serial. That emulator phase left real OTP untouched; physical power cuts and H755 hardware remain deferred.
- [x] Implement the H563 OTP adapter with guarded halfword reads, delayed ECC/NMI acknowledgement, scoped MPU permissions and shared controller ownership. Provisioning is disabled by default; automated HIL rejects programming-enabled builds. Read-only DUT qualification found block 0 programmed/locked and blocks 1–31 virgin/unlocked, stable across repeated scans/reset and regression tests. No real OTP writes or lock changes were performed during that read-only phase.
- [x] Perform the explicitly approved real-OTP write/lock validation: stored `dave_nucleoh563_sn001` in block 1 at `0x08FFF040`, verified all 32 halfwords programmed and the effective permanent lock. Block 0 and blocks 2–31 retained their fingerprints and locks. Restored read-only firmware and passed read-only HIL with serial/fingerprint/lock retention through factory programming and reset. All further automated real-OTP tests must remain read-only; further real writes require explicit user instruction. Physical power-cut tests remain deferred.
- [x] Final OTP regression: host 27/27, ASan/UBSan 27/27, fake 21/21, logging-disabled 28/28, TSan 26/26, GCC 13 28/28, networking 28/28; portable Python 18 passed (5 opt-in skips), three standalone starters, formatting/lint, H563/H755 ARM builds and console variants, and programming plans passed. Full H563 HIL with the flash emulator passed 30 tests (real-OTP-only test skipped); the separate real-OTP read-only HIL test passed, preserving the serial and every block fingerprint/lock. Left the DUT on provisioning-disabled real-OTP firmware. H755 hardware and physical power-cut testing remain deferred.
- [ ] Add device-key OTP record types and their provisioning policy later; serial number is the only initial payload.
- [x] Add the CRTP state-machine helper with deferred hooks, dwell/per-state statistics, read-only snapshots, and unit tests; convert the OTA Engine using a private nested machine.
- [x] Convert the journal, OTA writer/package reader/protocol, watchdog controller/confirmation, host FileFlash, and scheduler lifecycle to the shared state-machine helper. Keep state enums and implementation classes nested; derived status snapshots and persistent image metadata remain data. Validation: host 25/25, ASan/UBSan 25/25, fake 19/19, logging-disabled 26/26, TSan 24/24; H563/H755 ARM builds, format/lint, and H563 HIL 13/13 passed. Bootloader is 18,224 bytes of its 32 KiB reservation. H755 hardware remains deferred.

## Test organization

- [x] Keep Catch2 C++ tests and migrate Python tooling/process/starter runners to pytest; install the pinned test dependency in CI.
- [x] Group H563 console/network/UART/fault/watchdog/OTA tests under tests/hil with shared ownership, factory programming before every selected test, artifacts and JUnit reporting. All 13 HIL cases passed against the published Watcher pin; host 24/24, ASan 24/24, fake 18/18, logging-disabled 25/25, both ARM starters, format and lint passed.
- [x] Fix Watcher's serial read timeout being treated as EOF; publish 3f5ce6a with two PTY regression cases (37 tests passed, one optional skip), and pin the fix for HIL.
- [ ] Add a dedicated physical HIL CI runner once its board/probe/network ownership is arranged; ordinary GitHub CI remains hardware-free.

## Existing work

- [x] Add capacity-first application factories and document the raw-handler token-limit migration; keep explicit handler-name extraction tests.
- [x] Reflash and verify the final H563 image; pass another 120 command checks across UART/USB/TCP, two runs of the checked-in UART stress tool (1,160 burst commands), 1,400-byte ping, TCP reconnect, and software-reset recovery. All transmit counters remained clean; no visual LED/button confirmation in this run.
- [x] Fix the UART DMA completion race: a post-start control-register read/modify/write could re-enable an exhausted H563 GPDMA transfer (captured HAL_DMA_ERROR_USE). Leave HAL half-transfer handling enabled on both boards; reduce echo pressure by appending only new input bytes.
- [x] Repeat H563 hardware validation: the baseline failed 8/10 cycles; with both UART fixes, all 10 cycles passed 1,200 UART/USB/TCP command checks and 5,800 unpaced burst commands, with zero dropped frames/transmit errors. Add tests/hil/h563/uart_stress.py for repeatable UART regression checks. H755 remains build-tested only, with hardware testing deferred.

- [x] Default event-free modules/factories to NoEvent; add named application capacities and full logger constraints.
- [x] Compact command metadata into one shared static table per module and emit one specific diagnostic per adapter failure. With six typed parameters, 32-bit/float bounds and variant policies, at `88f03b5`, the H563 debug (-O0) full network-console image shrank by 7,016 flash bytes (text+data: 332,292 to 325,276) and 64 bytes of static RAM. ARM metadata: 28 bytes per argument, 32 bytes per compact command. Build measurement only.
- [x] Pin both starters to the NoEvent, named-capacity, and compact-command implementation commit.
- [x] Add enum-backed command choices, inferred from DAVEOS_ENUM or explicit label/value tables, using exact/unique-prefix lazy matching.
- [x] Flash H563 and verify typed/enum command parsing with 114 UART/USB/TCP checks, then Ethernet ping, TCP reconnect, and software-reset recovery. No visual LED confirmation in this run.

- [x] Implement typed command adapters in PROJECT.md: signature-derived count/types, trailing optional arguments, bounded numeric parsers, strict/friendly booleans, thin DAVEOS_COMMAND registration, and argument-aware help/errors.
- [x] Smoke-test Application composition on H563: UART/USB/TCP commands, timers, button reads, LED command acknowledgements, statistics, large ping, TCP reconnect, and software-reset recovery.
- [x] Add passive Application composition with post-init command binding and declarative periodic tasks with compile-time intervals and explicit overrides.
- [x] Pin both starters to the tested application/console API commit.
- [x] Replace enum events with allocation-free std::variant payloads, typed DAVEOS_EVENT registration, and an explicit visitor fallback; preserve interrupt-safe copied broadcast delivery.
- [x] Pin both starters to the variant-event implementation commit.
- [x] Add clang-format configuration consistent with PROJECT.md and a formatting check.
- [x] Add cppcheck configuration and a lint command; integrate both checks into CI.
- [x] Implement the STM32H563 platform, CubeMX startup/linker integration, and DaveOS LED example.
- [x] Bring H563 to H755 console parity with shared commands, UART echo, TX DMA, and reset.
- [x] Validate H563 HSI boot, physical LEDs/button, UART commands, TX DMA, software reset, and 200 ms timer completion on NUCLEO-H563ZI.
- [ ] Stress injected H563 UART errors and DMA overflow/recovery, precision timing, and extended sleep/wake behavior; burst and overlength recovery checks already pass.
- [ ] Explore a MISRA-friendly alternative to printf-style log formatting.
- [x] Add STM32 UART command input using application-owned line buffering (H755 M7).
- [x] Add STM32H755 support with pinned CubeH7 HAL, M7 console, and sleeping M4 image.
- [x] Confirm H755 dual-core startup, M4 sleep, M7 scheduler idle, and approximate TIM2 rate through OpenOCD/GDB.
- [ ] Complete H755 injected USART3/DMA error recovery, physical LED/button confirmation, timing-accuracy, and extended sleep/wake hardware validation. Current command and burst checks do not cover those physical/fault-injection cases.
- [x] Diagnose H755 newlib-nano `%llu` log-formatting HardFault; use toolchain full newlib for M7.
- [x] Audit STM32 ELF allocation paths and embedded formatting: H755 statistics previously grew the heap by 744 bytes. Remove floating-point statistics formatting and exception-pool startup dependencies; reject `_sbrk`, report allocation call sites at build time, and check zero heap requests on both boards. See [memory policy and audit](docs/memory.md).
- [x] Diagnose H755 clock mismatch (25 MHz assumed, 8 MHz measured); confirm 8 MHz ST-LINK rate, then select internal HSI and correct PLL settings as requested.
- [x] Add H755 USB CDC command/log console; verify enumeration, commands, and DTR session reset on hardware.
- [x] Validate H755 USB cable unplug/replug and command recovery (user hardware check).
- [ ] Validate H755 USB sustained backpressure recovery on hardware.
- [x] Add shared board timer command and verify 200 ms completion on H755 hardware.
- [x] Bring H563 USB console to H755 feature parity using shared CDC middleware and transport code.
- [x] Validate H563 USB enumeration, commands/logging, DTR reopen, and recovery after software reset.
- [x] Validate H563 USB-C attachment in both orientations and command recovery after physical reconnect.
- [ ] Stress H563 USB backpressure and host suspend/resume on hardware.
- [x] Add standalone lwIP IPv4 service, STM32 Ethernet drivers, and optional net module.
- [x] Validate H755 DHCP/static addressing, ping, cable reconnect, and USB console responsiveness on hardware.
- [x] Validate H563 DHCP, large-packet ping, and single-client TCP console on hardware.
- [x] Validate H563 DHCP/TCP recovery after Ethernet cable reconnection.
- [ ] Validate H563 static IPv4 on hardware.
- [x] Add a single-client TCP console on port 1000 with independent command/log registration.
- [ ] Generalize the standalone TCP API for multiple application listeners/connections.

- [x] Extract shared console helpers and CRTP module; make USB application-owned and consume TCP input in bounded chunks.

- [x] Fix the original H563 startup stack overflow by reserving 64 KiB; later move application objects to static storage.
- [x] Give the H563 stack all remaining contiguous SRAM above statics, with a 64 KiB minimum-headroom assertion rather than a fixed reservation. Add startup painting and hardware watermark checks; measurements are workload evidence, not worst-case qualification.
- [x] Hardware-test the current H755 firmware over UART, USB, and TCP, including static storage, FIFO/16-line input, Meson board/component selection, bound timers, and reusable command binding. Current reliability bring-up supersedes the earlier hardware deferral; standalone starter hardware testing remains outstanding.
- [x] Give H755 M7/M4 stacks their remaining contiguous RAM, remove the heap reservation, and retain minimum-headroom assertions. The full A/B M7 console has 17,200 stack bytes; Cortex-M7 has no MSPLIM guard. Revisit DTCM/AXI buffer placement before growing static allocations; see the measured workloads in [memory policy and audit](docs/memory.md).

- [x] Fix H563 UART burst overruns with hardware FIFO reception and a 16-line queue; validated 580 unpaced commands at 1 Mb/s, including 4,112-byte bursts and overlength recovery.

- [x] Consolidate STM32 examples into one Meson-selected board target (H563 default, H755 with sleeping M4) and remove application feature-selection macros.
- [x] Add task helpers, exact chrono delays, object-bound timers, reusable stage2 command binding, and retained initialization diagnostics.
- [x] Add a standalone host/fake application starter, exported Meson dependencies, and consumer build tests.
- [x] Validate the updated H563 firmware over UART/USB/TCP, including the bound timer command, Ethernet, and reset recovery.

- [x] Add ready-made host stdout subscriber and status-to-exit-code runner.
- [x] Promote STM32 console transports/commands and Nucleo board support to reusable dependencies; add the C++ Console group.
- [x] Add a standalone H563/H755 device starter and CI consumer-build/programming-plan checks.
- [x] Organize the documentation into hello, logging/commands, hardware console, and custom-component levels.
- [x] Validate the reusable console on H563 over UART/USB/TCP, including Ethernet ping and reset recovery; validate the standalone device starter's periodic worker, commands, and reset over UART. H755 has build/programming-plan coverage only for these changes.
- [x] Add daveos::util for CRC, version identity, and retained-fault records.
- [x] Add software CRC32 with injected hardware support and H563 hardware CRC implementation.
- [x] Add injected SPI/I2C HALs with callback completion, polling helper, aliased registries, per-controller ownership, statistics, fake backends, and H563/H755 IRQ adapters. [API guide](docs/spi-i2c.md). H563 read-only SD startup/OCR passed five times at 250 kHz; H755 bus adapters are build-tested only.
- [x] Remove task-index assumptions from watchdog HIL fault injection. H563 now locates `health.Heartbeat` by name and passes with the optional SD module; H755 uses the same helper but has not been hardware-rerun for this test change.
- [x] Validate H563 I2C1 PB8/PB9 with MCP3425 at 0x68: three address scans and 30 one-shot conversions, watchdog/fault and zero-heap checks passed. Optional `i2c_adc_probe` fixture provides `i2c scan`, `i2c stats`, and `adc sample`.
- [ ] Qualify H755 SPI/I2C with fixtures and additional SPI modes/speeds. Optimize the reusable SD reader beyond its fixed response capture window.
- [ ] Add SPI DMA after the fake and interrupt-driven H563/H755 HAL backends, particularly for SPI SD-card throughput. Preserve the portable transaction API; no I2C DMA work is currently planned.

## For STM32

- [x] Implement H563 A/B flash layout; see bootloader and reliability above.
- [x] Implement H563 bootloader selection by installation order and CRC-valid eligibility.
- [x] Implement cooperative inactive-slot OTA with chunk/final CRC, injected flash driver, and explicit flash layout.
- [x] Expand H563 `sd probe` with CSD/CID, CRC16-checked CMD17 reads, repeated 250 kHz/1 MHz comparisons, and read-only MBR/FAT BPB inspection. Five HIL probes passed (60 sector reads); connected 32 GB card reports FAT32, 32 KiB clusters. No card writes or filesystem mount.
- [x] Add read-only FatFs as a pinned submodule, injected block-device/volume APIs, file-backed host tests and an optional serialized filesystem module.
- [x] Add opt-in read-write mounting and new-file creation with sync/close,
  injected write/sync callbacks, host file writes and SPI response checks.
  H563 creation/remount/readback and overwrite refusal passed; 69 original-file
  read checks still pass with matching saved content samples. Two empty files
  from the initial busy-release diagnosis remain on the card; see storage docs.
- [x] Add `fs rm` / `Volume::remove`: files only, read-write mounts, copied
  command paths, metadata sync and open-handle/reentry protection. ASan/UBSan
  and logging-disabled storage tests passed; H563 create/remove/remount HIL
  passed with only its newly created temporary file removed.
- [ ] Qualify write power-loss/media-removal behavior; add
  multiblock writes and further file operations separately.
- [x] Add timer-driven I2C GPIO bus clear, explicit `i2c reset`, deferred
  startup recovery, immutable controller-level address probing, and visible
  counter overflow. Move MCP3425 into `daveos-drivers`.
- [x] H563 I2C HIL passed 2/2: three scans and 30 conversions, plus a real
  interrupted MCP3425 read recovered through both `i2c reset` and MCU startup.
  The injection verified SDA held low with MCU outputs released. No transaction
  timeouts, retained fault or heap attempts; observed stack use was 2,808 bytes.
- [ ] Qualify physical I2C clock stretching and repeated START with suitable
  fixtures; H755 I2C hardware remains unqualified.
- [ ] Implement a display driver module for ssd1306 devices using DI of the I2C via the layer above

- [x] Add task-only nested `yield()`, one eligible callback per call, bounded depth, context diagnostics and elapsed/self/nested accounting.
- [x] Integrate FatFs through a serialized filesystem worker and asynchronous SD reader that yields while waiting. Same-volume nested calls and competing requests fail immediately. See [storage](docs/storage.md).

- [x] Validate H563 read-only FatFs mount/list, missing-file errors, media reservation,
  unmount/remount and watchdog health. Both SPI HIL cases passed; stack watermark
  2,936 bytes and zero heap requests after filesystem work on the initially
  empty card. No card writes.
- [x] Validate populated-card reads on H563: 69 read requests across six files,
  subdirectories, long filename, 512-byte sector and 32 KiB cluster boundaries,
  4 KiB previews, EOF and past-EOF handling; repeated/overlapping reads agree.
  Watchdog/fault checks passed. Host tests retain fragmented-file coverage.
- [ ] Validate card-removal recovery, fragmented files on hardware, independent
  source-file comparisons and larger directory workloads.

- [x] Replace the fixed SD write delay with bounded SPI response polling,
  preserving CS, the original deadline, normal cleanup and per-window counters.
- [ ] Extract SD initialization from the example into storage/sd.
- [ ] Design a cooperative wait helper that distinguishes timeout from released
  peripheral-buffer ownership; add explicit watchdog/yield integration coverage.
