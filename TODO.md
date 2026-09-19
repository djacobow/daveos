# Follow-up work

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
