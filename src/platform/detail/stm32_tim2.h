// Include after the target CMSIS device header. Shared H5/H7 TIM2 backend.
#pragma once

#include <algorithm>

#include "daveos/platform/detail/stm32_tim2.h"

namespace daveos::platform::detail {
template <typename Derived>
void Stm32Tim2<Derived>::enter() {
  auto mask = __get_PRIMASK();
  __disable_irq();
  if (depth_++ == 0) saved_mask_ = mask;
}
template <typename Derived>
void Stm32Tim2<Derived>::leave() {
  if (--depth_ == 0) __set_PRIMASK(saved_mask_);
}
template <typename Derived>
core::Status Stm32Tim2<Derived>::init(std::uint32_t timer_hz) {
  core::Guard guard(*this);
  if (initialized_) return core::Status::already_initialized;
  if (timer_hz < 1000000 || timer_hz % 1000000 != 0 ||
      timer_hz / 1000000 > 65536)
    return core::Status::invalid_argument;
  RCC->APB1LENR = RCC->APB1LENR | RCC_APB1LENR_TIM2EN;
  (void)RCC->APB1LENR;
  RCC->APB1LLPENR = RCC->APB1LLPENR | RCC_APB1LLPENR_TIM2LPEN;
  RCC->APB1LRSTR = RCC->APB1LRSTR | RCC_APB1LRSTR_TIM2RST;
  RCC->APB1LRSTR = RCC->APB1LRSTR & ~RCC_APB1LRSTR_TIM2RST;
  TIM2->CR1 = 0;
  TIM2->PSC = timer_hz / 1000000 - 1;
  TIM2->ARR = 0xffffffffu;
  TIM2->EGR = TIM_EGR_UG;
  TIM2->SR = 0;
  TIM2->DIER = TIM_DIER_UIE;
  epoch_ = 0;
  initialized_ = true;
  NVIC_ClearPendingIRQ(TIM2_IRQn);
  NVIC_SetPriority(TIM2_IRQn, (1u << __NVIC_PRIO_BITS) - 1);
  NVIC_EnableIRQ(TIM2_IRQn);
  TIM2->CR1 = TIM_CR1_CEN;
  return core::Status::ok;
}
template <typename Derived>
core::Time Stm32Tim2<Derived>::now() {
  core::Guard guard(*this);
  auto low = TIM2->CNT;
  // Sample the counter again if rollover happened before or during the read.
  // Interrupts must not be masked for a full 32-bit period (about 71 minutes).
  if (TIM2->SR & TIM_SR_UIF) {
    TIM2->SR = ~TIM_SR_UIF;
    epoch_ += core::Time{1} << 32;
    low = TIM2->CNT;
  }
  return epoch_ + low;
}
template <typename Derived>
void Stm32Tim2<Derived>::ProgramCompare() {
  TIM2->DIER = TIM_DIER_UIE;
  TIM2->SR = ~TIM_SR_CC1IF;
  auto current = now();
  auto delay = due_ > current ? due_ - current : 0;
  auto target = current + std::min(delay, core::Time{0x7fffffff});
  TIM2->CCR1 = static_cast<std::uint32_t>(target);
  TIM2->DIER = TIM_DIER_UIE | TIM_DIER_CC1IE;
  // The compare may have passed while programming it, including delay zero.
  if (now() >= target) NVIC_SetPendingIRQ(TIM2_IRQn);
}
template <typename Derived>
void Stm32Tim2<Derived>::arm(core::Time delay, Callback callback,
                             void* argument) {
  core::Guard guard(*this);
  callback_ = callback;
  argument_ = argument;
  due_ = core::After(now(), delay);
  ProgramCompare();
}
template <typename Derived>
void Stm32Tim2<Derived>::disarm() {
  core::Guard guard(*this);
  if (!initialized_) return;
  TIM2->DIER = TIM_DIER_UIE;
  TIM2->SR = ~TIM_SR_CC1IF;
  callback_ = nullptr;
  due_ = core::kForever;
}
template <typename Derived>
void Stm32Tim2<Derived>::quiesce() {
  core::Guard guard(*this);
  if (!initialized_) return;
  disarm();
  NVIC_DisableIRQ(TIM2_IRQn);
  NVIC_ClearPendingIRQ(TIM2_IRQn);
  TIM2->DIER = 0;
  TIM2->CR1 = 0;
  initialized_ = false;
}
template <typename Derived>
void Stm32Tim2<Derived>::interrupt() {
  Callback callback = nullptr;
  void* argument = nullptr;
  {
    core::Guard guard(*this);
    if (!initialized_) return;
    auto current = now();
    TIM2->SR = ~TIM_SR_CC1IF;
    if (callback_ && current >= due_) {
      callback = callback_;
      argument = argument_;
      disarm();
    } else if (callback_) {
      ProgramCompare();
    }
  }
  if (callback) callback(argument);
  notify();
}
template <typename Derived>
bool Stm32Tim2<Derived>::in_interrupt() const {
  return __get_IPSR() != 0;
}
template <typename Derived>
core::Context Stm32Tim2<Derived>::context() const {
  return in_interrupt() ? core::Context{"core", "interrupt"} : context_;
}
template <typename Derived>
void Stm32Tim2<Derived>::context(core::Context value) {
  if (!in_interrupt()) context_ = value;
}
template <typename Derived>
std::uint64_t Stm32Tim2<Derived>::sequence() {
  core::Guard guard(*this);
  return sequence_;
}
template <typename Derived>
void Stm32Tim2<Derived>::notify() {
  core::Guard guard(*this);
  ++sequence_;
}
template <typename Derived>
void Stm32Tim2<Derived>::idle(core::Time deadline, bool sleep,
                              std::uint64_t observed) {
  core::Guard guard(*this);
  if (sequence_ != observed || now() >= deadline) return;
  if (sleep) {
    // Check and WFI are atomic with respect to ISR scheduling. A pending NVIC
    // interrupt wakes WFI under PRIMASK; leave() then allows its handler to
    // run.
    SCB->SCR = SCB->SCR & ~SCB_SCR_SLEEPDEEP_Msk;
    __DSB();
    __WFI();
    __ISB();
  }
  // Awake waiting returns to the scheduler to check queues and deadlines again.
}
}  // namespace daveos::platform::detail
