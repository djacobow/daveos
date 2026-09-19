# Follow-up work

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
- [ ] Hardware-test the current H755 firmware over UART, USB, and TCP, including static storage, FIFO/16-line input, Meson board/component selection, bound timers, and reusable command binding; currently build coverage only.
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
