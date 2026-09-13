#pragma once

#include "daveos/platform/detail/stm32_tim2.h"

namespace daveos::platform::stm32h5 {


// Single-core TIM2 adapter; see detail::Stm32Tim2 for the peripheral contract.
class Platform final : public detail::Stm32Tim2<Platform> {};


}  // namespace daveos::platform::stm32h5
