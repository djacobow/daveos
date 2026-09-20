# Follow-up work

## Bootloader and reliability implementation

- [x] Wire the optional H563 TCP OTA listener (1001), enable/disable/status commands, and delayed reboot callback. On hardware, reject a bad chunk, abort on disconnect, then install A→B and B→A over TCP; each transfer took about 35.45 s including flash CRC and metadata commit. Concurrent console timer requests passed (167/172 checks, maximum 13 ms response); both reboot requests, trial boots, and manual confirmations passed. Add fragmented-protocol and failed-final-commit regression tests. Automatic health-based confirmation remains outstanding.
- [x] Repeat the TCP round trip with the final build: two more installations/reboots/confirmations passed (35.46 s each), with 331 concurrent console timer checks and maximum 13 ms response. Leave A confirmed and OTA disabled. ASan passed 24/24, format/lint passed, and the standalone H755 firmware still builds. H755 hardware remains deferred.
- [x] Link H563 slot B from the same application objects as A; build a paired relocation package that reconstructs B exactly. On hardware, verify B trial boot, unconfirmed-reset rejection and fallback to A, durable/idempotent `boot confirm`, three confirmed-B reset cycles, and CRC rejection/fallback after erasing B's first sector. Restore and leave B confirmed; preserve A. Fix ICACHE-stale metadata readback after flash programming. These installations used ST-LINK; TCP OTA and automatic health-based confirmation remain outstanding.
- [x] Repeat five software resets of the restored confirmed B: UART/USB/TCP timers, DHCP, and 1,400-byte ping passed after every boot. ASan passed 24/24, including paired-package layout/version checks. Current linked applications are 217,796 bytes each; the paired package is 223,714 bytes.
- [x] Build an ST-LINK-programmable H563 factory HEX with the fixed 32 KiB bootloader, confirmed slot A, and redundant initial metadata; mass-erase main flash before programming, preserving OTP. First hardware boot verified the CRC and started the relocated application successfully.
- [x] Validate the H563 factory boot through five software-reset cycles: CRC verification, UART/USB/TCP commands and timers, DHCP, and 1,400-byte ping passed in every cycle. Fix a false UART start bit by enabling the transmitter before selecting the TX pin alternate function. Bootloader uses 17,404 bytes; slot A uses 202,368 bytes. ASan passed 24/24; formatting/lint and standalone H755 build passed. H755 hardware remains deferred; OTA and slot-B hardware validation are still outstanding.
- [ ] Implement the bootloader/OTA specification in PROJECT.md; validate H563 and host/fake first. H755 bootloader integration and hardware validation remain deferred.
- [ ] Use `-Os` for every STM32 compilation, including dependencies, M4, and device starters; retain debug information.
- [ ] Add independent application/bootloader version stamps from Meson: u32 major/minor/CI build, local UINT32_MAX sentinel, Git commit and dirty flag.
- [ ] Implement software and injected STM32 CRC-32 matching Python binascii.crc32, including caller-owned incremental state and software fallback from interrupts.
- [ ] Add injected flash operations, redundant metadata journal, one-trial boot selection, explicit durable confirmation, and corruption/power-loss tests.
- [ ] Measure the complete bootloader at `-Os` and enforce its fixed 32 KiB reservation. Layout: bank 1 [BL][metaA][A], bank 2 [32 KiB placeholder][metaB][B]; metadata sectors are 8 KiB each, A/B capacities are 984 KiB each.
- [ ] Prove block-local device-side relocation against both independently linked images and on H563; use separate slot-specific images if feasibility fails.
- [ ] Add transport-independent incremental OTA, a dedicated TCP listener on port 1001, Python uploader with optional --reboot, and application-controlled enable/disable/status.
- [ ] Add an injected, explicitly started watchdog with latched health/feed failures, early startup coverage, scheduler progress checking, and debugger freeze.
- [ ] Add one magic/version/size/CRC-protected retained fault record, shared by bootloader and application; capture faults before debugger break or reset.
- [ ] Repeatedly validate A/B updates, confirmation, rejection/rollback, flash corruption, initialization failures, watchdogs, and console responsiveness on H563.
- [ ] Add an OTP driver for serial numbers and optional device keys/other provisioning data; design provisioning and locking separately.
- [ ] Audit and convert existing state machines to the AGENTS.md enum/cs/ns/switch/single-commit structure in separate work.

## Existing work

- [x] Add capacity-first application factories and document the raw-handler token-limit migration; keep explicit handler-name extraction tests.
- [x] Reflash and verify the final H563 image; pass another 120 command checks across UART/USB/TCP, two runs of the checked-in UART stress tool (1,160 burst commands), 1,400-byte ping, TCP reconnect, and software-reset recovery. All transmit counters remained clean; no visual LED/button confirmation in this run.
- [x] Fix the UART DMA completion race: a post-start control-register read/modify/write could re-enable an exhausted H563 GPDMA transfer (captured HAL_DMA_ERROR_USE). Leave HAL half-transfer handling enabled on both boards; reduce echo pressure by appending only new input bytes.
- [x] Repeat H563 hardware validation: the baseline failed 8/10 cycles; with both UART fixes, all 10 cycles passed 1,200 UART/USB/TCP command checks and 5,800 unpaced burst commands, with zero dropped frames/transmit errors. Add tools/hardware/uart_stress.py for repeatable UART regression checks. H755 remains build-tested only, with hardware testing deferred.

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
