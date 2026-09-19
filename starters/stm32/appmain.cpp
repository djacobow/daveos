#include "appmain.h"

#include "core/logging/logger.hpp"
#include "core/schedule/application.hpp"
#include "platform/stm32/console/board.hpp"
#include "platform/stm32/console/console.hpp"
#include "platform/stm32/console/uart.hpp"
#include "worker.hpp"

namespace app {
  namespace stm32 = daveos::platform::stm32;
  board::Platform platform;
  stm32::UartConsole<Event> uart{platform};
  stm32::Console console{uart};

  void Statistics(core::SchedulerInterface<Event>& scheduler) {
    console.log_statistics(scheduler);
  }

  stm32::Board<Event> board_module{platform, Statistics};
  Worker worker;
  auto logger = core::make_logger(platform, console.subscribers());
  auto application = core::make_application<Event>(
      platform, console.modules(board_module, worker), logger,
      console.sources());
  board::Platform* active_platform = nullptr;
  volatile core::Status last_status = core::Status::ok;
  core::InitializationFailure initialization_failure;
}  // namespace app

extern "C" void TIM2_IRQHandler() {
  if (app::active_platform) {
    app::active_platform->interrupt();
  }
}

// CubeMX-generated main() calls this after clocks and USART3 are configured.
extern "C" void appmain() {
  app::last_status = app::platform.init(board::TimerClock());
  if (app::last_status != app::core::Status::ok) {
    Error_Handler();
  }
  app::active_platform = &app::platform;
  app::last_status = app::application.run();
  app::initialization_failure = app::application.initialization_failure();
  app::console.stop();
  app::platform.quiesce();
  app::active_platform = nullptr;
  Error_Handler();
}
