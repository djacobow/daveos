#include <cstdio>

#include "daveos/core/logger.h"
#include "daveos/core/scheduler.h"
#ifdef DAVEOS_FAKE
#include "daveos/platform/fake/platform.h"
using Platform = daveos::platform::fake::Platform;
#else
#include "daveos/platform/host/platform.h"
using Platform = daveos::platform::host::Platform;
#endif

namespace app {
using namespace daveos::core;
enum class Event { hello };
class Hello final : public Module<Hello, Event> {
 public:
  static constexpr const char* name() { return "hello"; }
  static constexpr auto tasks() {
    return std::array{TaskDescriptor<Hello>{"greet", &Hello::greet}};
  }
  Status init(InitStage stage) {
    if (stage == InitStage::stage1)
      return scheduler().schedule(*this, &Hello::greet, 1000);
    return Status::ok;
  }
  void greet() {
    I_("Hello, DaveOS!");
    scheduler().stop();
  }
};
void Output(void*, const LogRecord& record) {
  std::printf("[%llu] %s/%s: %.*s\n",
              static_cast<unsigned long long>(record.timestamp), record.module,
              record.task, static_cast<int>(record.message.size()),
              record.message.data());
}
}  // namespace app
int main() {
  Platform platform;
  app::Hello hello;
  auto logger = daveos::core::make_logger(
      platform, daveos::core::SubscriberList{
                    daveos::core::Subscriber{nullptr, app::Output}});
  auto scheduler = daveos::core::make_scheduler<app::Event>(
      platform, daveos::core::ModuleList{&hello}, logger);
  return scheduler.run() == daveos::core::Status::ok ? 0 : 1;
}
