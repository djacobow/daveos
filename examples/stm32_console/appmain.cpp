#include "appmain.h"

#include "composition.hpp"
#include "core/logging/logger.hpp"
#include "core/schedule/application.hpp"

namespace app {
  // Constructors only store references and metadata. Module init performs
  // setup.
  Platform platform;
  Components components{platform};

  void LogTransportStatistics(core::SchedulerInterface<Event>& scheduler) {
    components.log_statistics(scheduler);
  }

  Board board_module{platform, LogTransportStatistics};
  auto modules = components.modules(board_module);
  auto logger = core::make_logger(platform, components.subscribers());
  auto application = core::make_application<Event>(platform, modules, logger,
                                                   components.sources());

  // TIM2 is routed only after platform initialization succeeds.
  Platform* active_platform = nullptr;
  // Retained for debugger inspection even if a transport failed to initialize.
  volatile core::Status last_status = core::Status::ok;
  core::InitializationFailure initialization_failure;
}  // namespace app

extern "C" void TIM2_IRQHandler() {
  if (app::active_platform) {
    app::active_platform->interrupt();
  }
}

extern "C" void appmain() {
  app::last_status = app::platform.init(board::TimerClock());
  if (app::last_status != app::core::Status::ok) {
    Error_Handler();
  }
  app::active_platform = &app::platform;
  app::last_status = app::application.run();
  app::initialization_failure = app::application.initialization_failure();
  // Initialization failure is terminal. Release transports before halting.
  app::components.stop();
  app::platform.quiesce();
  app::active_platform = nullptr;
  Error_Handler();
}
