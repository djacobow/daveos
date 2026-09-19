#pragma once

#include <csetjmp>
#include <cstdint>

// Minimal register model for exercising the real adapter on the host.
namespace hardware {
inline std::uint32_t compare_latency = 0;

struct CompareRegister {
  std::uint32_t value = 0;

  operator std::uint32_t() const { return value; }

  void operator=(std::uint32_t next);
};

struct StatusRegister {
  std::uint32_t flags = 0;

  operator std::uint32_t() const { return flags; }

  void operator=(std::uint32_t value) {
    flags &= value;
  }  // write zero to clear
};

struct Timer {
  std::uint32_t CR1 = 0, PSC = 0, ARR = 0, EGR = 0, DIER = 0, CNT = 0;
  CompareRegister CCR1;
  StatusRegister SR;
};

struct Clock {
  std::uint32_t APB1LENR = 0, APB1LLPENR = 0, APB1LRSTR = 0;
};

struct System {
  std::uint32_t SCR = 0;
};

inline Timer timer;

inline void CompareRegister::operator=(std::uint32_t next) {
  value = next;
  timer.CNT += compare_latency;
}

inline Clock clock;
inline System system;
inline std::uint32_t mask = 0, ipsr = 0, sleeps = 0, sleep_mask = 0;
inline bool pending = false, enabled = false;

inline void Reset() {
  timer = Timer{};
  clock = Clock{};
  system = System{};
  mask = ipsr = sleeps = sleep_mask = 0;
  compare_latency = 0;
  pending = enabled = false;
}
}  // namespace hardware

inline auto* TIM2 = &hardware::timer;
inline auto* RCC = &hardware::clock;
inline auto* SCB = &hardware::system;
inline constexpr std::uint32_t RCC_APB1LENR_TIM2EN = 1,
                               RCC_APB1LLPENR_TIM2LPEN = 1,
                               RCC_APB1LRSTR_TIM2RST = 1, TIM_EGR_UG = 1,
                               TIM_DIER_UIE = 1, TIM_DIER_CC1IE = 2,
                               TIM_CR1_CEN = 1, TIM_SR_UIF = 1,
                               TIM_SR_CC1IF = 2, SCB_SCR_SLEEPDEEP_Msk = 4,
                               __NVIC_PRIO_BITS = 4;
inline constexpr int TIM2_IRQn = 45;

inline std::uint32_t __get_PRIMASK() { return hardware::mask; }

inline void __disable_irq() { hardware::mask = 1; }

inline void __set_PRIMASK(std::uint32_t value) { hardware::mask = value; }

inline std::uint32_t __get_IPSR() { return hardware::ipsr; }

inline void NVIC_ClearPendingIRQ(int) { hardware::pending = false; }

inline void NVIC_SetPendingIRQ(int) { hardware::pending = true; }

inline void NVIC_SetPriority(int, std::uint32_t) {}

inline void NVIC_EnableIRQ(int) { hardware::enabled = true; }

inline void NVIC_DisableIRQ(int) { hardware::enabled = false; }

inline void __DSB() {}

inline void __ISB() {}

inline void __WFI() {
  hardware::sleep_mask = hardware::mask;
  ++hardware::sleeps;
}

// Model the non-returning hardware boundary without enabling exceptions.
namespace hardware {
inline std::jmp_buf reset_destination;
}

[[noreturn]] inline void NVIC_SystemReset() {
  std::longjmp(hardware::reset_destination, 1);
}
