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
  CommandWiring command_wiring;
  auto modules = components.modules(command_wiring, board_module);
  auto logger = core::make_logger(platform, components.subscribers());
  auto scheduler = core::make_scheduler<Event>(platform, modules, logger);
  core::CommandDispatcher dispatcher(modules, scheduler);

  core::Status CommandWiring::init(core::InitStage stage) {
    if (stage == core::InitStage::stage2) {
      dispatcher.bind_sources(components.sources());
    }
    return core::Status::ok;
  }

  // TIM2 is routed only after platform initialization succeeds.
  Platform* active_platform = nullptr;
}  // namespace app

extern "C" void TIM2_IRQHandler() {
  if (app::active_platform) {
    app::active_platform->interrupt();
  }
}

extern "C" void appmain() {
  if (app::platform.init(board::TimerClock()) != app::core::Status::ok) {
    Error_Handler();
  }
  app::active_platform = &app::platform;
  app::scheduler.run();
  // Initialization failure is terminal. Release transports before halting.
  app::components.stop();
  app::platform.quiesce();
  app::active_platform = nullptr;
  Error_Handler();
}
