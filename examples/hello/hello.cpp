#include <cstdio>

#include "core/logging/logger.hpp"
#include "core/schedule/scheduler.hpp"
#include "platform/host/io.hpp"
#ifdef DAVEOS_FAKE
#include "platform/fake/platform.h"
using Platform = daveos::platform::fake::Platform;
#else
#include "platform/host/platform.h"
using Platform = daveos::platform::host::Platform;
#endif

namespace core = daveos::core;

namespace app {
  using std::chrono_literals::operator""ms;

  using Event = std::variant<std::monostate>;

  class Hello final : public core::Module<Hello, Event> {
   public:
    static constexpr const char* name() { return "hello"; }

    static constexpr auto tasks() {
      return std::array{DAVEOS_TASK(Hello, greet)};
    }

    core::Status init(core::InitStage stage) {
      if (stage == core::InitStage::stage1) {
        return schedule<&Hello::greet>(1ms);
      }
      return core::Status::ok;
    }

    void greet() {
      I_("Hello, DaveOS!");
      scheduler().stop();
    }
  };

  // Passive module construction; scheduler init() binds and initializes them.
  Hello hello;
}  // namespace app

int main() {
  Platform platform;
  auto logger = daveos::core::make_logger(
      platform, daveos::core::SubscriberList{
                    daveos::platform::host::stdout_subscriber()});
  auto scheduler = daveos::core::make_scheduler<app::Event>(
      platform, daveos::core::ModuleList{&app::hello}, logger);
  return daveos::platform::host::run(scheduler);
}
