# Follow-up work

- [x] Add clang-format configuration consistent with PROJECT.md and a formatting check.
- [x] Add cppcheck configuration and a lint command; integrate both checks into CI.
- [x] Implement the STM32H563 platform, CubeMX startup/linker integration, and DaveOS LED example.
- [x] Bring H563 to H755 console parity with shared commands, UART echo, TX DMA, and reset.
- [ ] Validate H563 HSI boot, LED/button commands, UART RX/error recovery, TX DMA/overflow, reset, timer timing, and sleep/wake on NUCLEO-H563ZI hardware.
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
