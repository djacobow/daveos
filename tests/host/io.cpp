#include "platform/host/io.hpp"

#include "core/logging/logger.hpp"
#include "core/schedule/application.hpp"
#include "platform/fake/platform.h"

namespace core = daveos::core;
namespace host = daveos::platform::host;
enum class Event {};

struct Hello : core::Module<Hello, Event> {
  static constexpr const char* name() { return "hello"; }

  static constexpr auto tasks() {
    return std::array{
        DAVEOS_PERIODIC(Hello, Poll, std::chrono::milliseconds{1})};
  }

  void Poll() {
    I_("Hello");
    (void)scheduler().stop();
  }
};

int main(int argc, char**) {
  if (argc > 1) {
    struct Failure {
      core::Status run() { return core::Status::initialization_failed; }
    } failure;

    return host::run(failure);
  }
  daveos::platform::fake::Platform platform;
  Hello hello;
  auto logger = core::make_logger(
      platform, core::SubscriberList{host::stdout_subscriber()});
  auto app =
      core::make_application<Event>(platform, core::ModuleList{&hello}, logger);
  return host::run(app);
}
