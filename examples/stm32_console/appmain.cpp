#include "appmain.h"

#include "composition.hpp"
#include "core/command/command.hpp"
#include "core/logging/logger.hpp"
#include "core/schedule/scheduler.hpp"

namespace app {
  // Constructors only store references and metadata. Module init performs
  // setup.
  Platform platform;
  Components components{platform};

  void LogTransportStatistics(core::SchedulerInterface<Event>& scheduler) {
    components.log_statistics(scheduler);
  }

  Board board_module{platform, LogTransportStatistics};
  auto command_wiring = core::make_command_binding<Event>(components.sources());
  auto modules = components.modules(command_wiring, board_module);
  auto logger = core::make_logger(platform, components.subscribers());
  auto scheduler = core::make_scheduler<Event>(platform, modules, logger);
  core::CommandDispatcher dispatcher(modules, scheduler);

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
  app::command_wiring.connect(app::dispatcher);
  app::last_status = app::scheduler.run();
  app::initialization_failure = app::scheduler.initialization_failure();
  // Initialization failure is terminal. Release transports before halting.
  app::components.stop();
  app::platform.quiesce();
  app::active_platform = nullptr;
  Error_Handler();
}
