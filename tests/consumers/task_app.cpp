// A task-only application: the kernel alone, with no logger, commands
// transports or other services.
#include "core/schedule/application.hpp"
#include "platform/fake/platform.h"

namespace core = daveos::core;

namespace {
  struct Worker : core::Module<Worker> {
    static constexpr const char* name() { return "worker"; }

    static constexpr auto tasks() {
      return std::array{
          DAVEOS_PERIODIC(Worker, Tick, std::chrono::milliseconds{1})};
    }

    void Tick() {
      if (++ticks == 3) {
        (void)scheduler().stop();
      }
    }

    int ticks = 0;
  };

  Worker worker;
}  // namespace

int main() {
  daveos::platform::fake::Platform platform;
  auto app = core::make_application(platform, core::ModuleList{&worker});
  return app.run() == core::Status::ok && worker.ticks == 3 ? 0 : 1;
}
