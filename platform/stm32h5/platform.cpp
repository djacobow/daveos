#include "platform/stm32h5/platform.h"

#include "stm32h563xx.h"

// Device definitions must precede the shared implementation.
#include "../detail/stm32_tim2_impl.hpp"

template class daveos::platform::detail::Stm32Tim2<
    daveos::platform::stm32h5::Platform>;
