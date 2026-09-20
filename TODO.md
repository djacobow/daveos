# Follow-up work

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

### Next: startup and runtime health

- [x] Start H563 IWDG before clock/peripheral/module initialization; reset on explicit init failure and let startup hangs expire.
- [x] Feed only after heartbeat and every active repeating task's progress checks; latch failures and preserve their diagnostics across reset.
- [x] Automatically confirm a trial after five seconds of healthy scheduling; USB/Ethernet/DHCP availability must not gate confirmation.
- [x] Validate H563 debugger freeze, latched task-rate failure, an interrupt-enabled runtime hang, early-warning exception-frame/CRC capture, natural watchdog reset, retained reporting, and automatic trial confirmation.
- [x] Stall H563 trial B in SystemClock_Config before clock setup: verify IWDG early-warning frame/CRC, natural reset, rejection of unconfirmed B, and healthy confirmed-A fallback.
- [ ] Add dedicated interrupt-masked-hang assertions (reset without a new frame), invalid-stack/PSP fault cases, and fault recovery without an attached debugger.
- [x] Report retained watchdog/init failures on startup and provide health status/fault/clear commands.
- [x] Integrate H563 application HardFault/BusFault/MemManage/UsageFault handlers with retained capture; trigger all four CPU faults on hardware and verify type/status bits, frame PC, CRC, reset and next-boot reporting. Add tests/hil/h563/test_faults.py for repeatable injection. ASan 24/24, format/lint, H563 boot/standalone/starter builds pass.
- [x] IWDG validation: ASan 24/24, fake 18/18, formatting/lint, H563 boot/standalone and H755 builds; OTA with the watchdog active and automatic confirmation on H563. H755 hardware remains deferred.

### Remaining qualification and follow-ups

- [ ] Complete independent bootloader version stamping (CI build, Git commit/dirty), expose application identity, and verify CI version propagation.
- [ ] Exercise hardware journal rollover, both-images-invalid recovery, physical power interruption, and comprehensive OTA timeout/link-loss/disable/replacement cases.
- [ ] Run the full host/fake/sanitizer matrix, format/lint, and H563/H755/starter builds before pushing. H755 bootloader integration and hardware testing remain deferred.
- [ ] Add an OTP driver for serial numbers and optional device keys; design provisioning/locking separately.
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
- [ ] Complete H755 USART3 input/error recovery, LED/button, timing-accuracy, and extended sleep/wake hardware validation.
- [x] Diagnose H755 newlib-nano `%llu` log-formatting HardFault; use toolchain full newlib for M7.
- [ ] Validate embedded formatting heap use with full newlib.
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
- [ ] Measure H563 whole-program stack high-water usage under console/network/interrupt load, then right-size the retained 64 KiB reservation in both linker and CubeMX settings.
- [ ] Deferred by user: hardware-test the current H755 firmware over UART, USB, and TCP, including static storage, FIFO/16-line input, Meson board/component selection, bound timers, and reusable command binding; currently build coverage only.
- [ ] Revisit H755 stack reservation after the application-owned console refactor; its Cortex-M7 has no MSPLIM guard.

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
- [ ] create DaveOS::Util space
- [ ] add crc32 driver to DaveOS::Util, perhaps allow for DI allow a hardware implementation if one is available as in most stm32 chips
- [ ] create a hal layer that allows us to abstract away SPI and I2C devices independent of vendor HAL, using DI to be part of "platform". hal layer should support interrupt based api and a polling based api, but not a blocking api

## For STM32

- [ ] Implement flash layout that support an A/B bootloader
- [ ] Implement simple bootloader that chooses latest valid image and starts it
- [ ] Implement a module that can perform an OTA to the opposite flash A/B bank. We'll want a protocol that crc's chunks as they come, as well as finally checking the whole image. The actual writing to flash should be via routines that come from the platform. Details about an image size, flash boundaries etc, should come from knowledge TBD
- [ ] Implement a FAT filesystem reader/writer module using FATFS (as a git submodule), again with DI of the SPI bus via the hal layer mentioned above
- [ ] Implememnt a display driver module for ssd1306 devices using DI of the I2C via the layer above
