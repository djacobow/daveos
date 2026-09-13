#pragma once

#include "platform/detail/stm32_tim2.hpp"

namespace daveos::platform::stm32h7 {


// Single-core TIM2 adapter; see detail::Stm32Tim2 for the peripheral contract.
// H755 uses this adapter on the M7 only; the M4 must not access TIM2.
class Platform final : public detail::Stm32Tim2<Platform> {};


}  // namespace daveos::platform::stm32h7
