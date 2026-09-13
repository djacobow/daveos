# Follow-up work

- [x] Add clang-format configuration consistent with PROJECT.md and a formatting check.
- [x] Add cppcheck configuration and a lint command; integrate both checks into CI.
- [x] Implement the STM32H563 platform, CubeMX startup/linker integration, and DaveOS LED example.
- [ ] Validate STM32H563 blinking, timer timing, and sleep/wake on the NUCLEO-H563ZI hardware.
- [ ] Explore a MISRA-friendly alternative to printf-style log formatting.
- [x] Add STM32 UART command input using application-owned line buffering (H755 M7).
- [x] Add STM32H755 support with pinned CubeH7 HAL, M7 console, and sleeping M4 image.
- [x] Confirm H755 dual-core startup, M4 sleep, M7 scheduler idle, and approximate TIM2 rate through OpenOCD/GDB.
- [ ] Complete H755 USART3 input/error recovery, LED/button, timing-accuracy, and extended sleep/wake hardware validation.
- [x] Diagnose H755 newlib-nano `%llu` log-formatting HardFault; use toolchain full newlib for M7.
- [ ] Validate embedded formatting heap use with full newlib.
- [x] Diagnose H755 clock mismatch (25 MHz assumed, 8 MHz measured); confirm 8 MHz ST-LINK rate, then select internal HSI and correct PLL settings as requested.
