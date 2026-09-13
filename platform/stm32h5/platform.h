#pragma once

#include "platform/detail/stm32_tim2.hpp"

namespace daveos::platform::stm32h5 {


// Single-core TIM2 adapter; see detail::Stm32Tim2 for the peripheral contract.
class Platform final : public detail::Stm32Tim2<Platform> {};


}  // namespace daveos::platform::stm32h5
