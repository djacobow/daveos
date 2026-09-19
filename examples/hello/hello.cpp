#include <cstdio>

#include "core/logging/log_format.hpp"
#include "core/logging/logger.hpp"
#include "core/schedule/scheduler.hpp"
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

  enum class Event { hello };

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

  void Output(void*, const core::LogRecord& record) {
    core::LogPrefix prefix(record);
    auto text = prefix.view();
    std::printf("%.*s%.*s\n", static_cast<int>(text.size()), text.data(),
                static_cast<int>(record.message.size()), record.message.data());
  }

  // Passive module construction; scheduler init() binds and initializes them.
  Hello hello;
}  // namespace app

int main() {
  Platform platform;
  auto logger = daveos::core::make_logger(
      platform, daveos::core::SubscriberList{
                    daveos::core::Subscriber{nullptr, app::Output}});
  auto scheduler = daveos::core::make_scheduler<app::Event>(
      platform, daveos::core::ModuleList{&app::hello}, logger);
  const auto status = scheduler.run();
  if (status != core::Status::ok) {
    std::fprintf(stderr, "DaveOS: %s\n", core::enum_name(status));
    return 1;
  }
  return 0;
}
