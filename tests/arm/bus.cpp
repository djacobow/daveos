#if defined(STM32H563xx)
#include "platform/stm32h5/bus.h"
namespace target = daveos::platform::stm32h5;
#else
#include "platform/stm32h7/bus.h"
namespace target = daveos::platform::stm32h7;
#endif
#include "core/schedule/scheduler.hpp"
#include "hal/adapters/daveos.hpp"
#include "hal/controller.hpp"

// Explicit instantiation compiles all paths, including error cleanup and the
// 32-bit, interrupt-masked counter implementation. No hardware is accessed.
struct CompileClock {
  std::uint64_t now() const;
  daveos::hal::Status arm(std::uint64_t, daveos::core::TimerCallback);
  void cancel(daveos::core::TimerCallback);
};
template class daveos::hal::Controller<target::SpiBus, CompileClock,
                                       target::BusCritical, 2>;
template class daveos::hal::Controller<target::I2cBus, CompileClock,
                                       target::BusCritical, 2>;
