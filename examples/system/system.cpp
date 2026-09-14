#include <cstdint>
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

namespace app {
using namespace daveos::core;
enum class Event { pulse };
class Producer final : public Module<Producer, Event> {
 public:
  static constexpr const char* name() { return "producer"; }
  static constexpr auto tasks() {
    return std::array{
        TaskDescriptor<Producer>{"pulse", &Producer::pulse},
        TaskDescriptor<Producer>{"complete", &Producer::complete}};
  }
  Status init(InitStage stage) {
    if (stage == InitStage::stage1) {
      active_ = this;
      return scheduler().schedule(*this, &Producer::pulse, 1000, Mode::repeat);
    }
    return Status::ok;
  }
  void pulse() {
    ++count_;
    I_("pulse %u", count_);
    scheduler().post(Event::pulse, this);
    if (count_ == 3) {
      scheduler().cancel(*this, &Producer::pulse);
      scheduler().timer(500, FinishTimer);
    }
  }
  void complete() {
    scheduler().log_statistics();
    scheduler().stop();
  }

 private:
  static void FinishTimer() {
    active_->scheduler().schedule(*active_, &Producer::complete, 0);
  }
  inline static Producer* active_ = nullptr;
  std::uint32_t count_ = 0;
};
class Consumer final : public Module<Consumer, Event> {
 public:
  static constexpr const char* name() { return "consumer"; }
  static constexpr auto tasks() {
    return std::array{TaskDescriptor<Consumer>{"report", &Consumer::report}};
  }
  void on_event(Event) { scheduler().schedule(*this, &Consumer::report, 0); }
  void report() { I_("received pulse"); }
};
void Output(void*, const LogRecord& record) {
  LogPrefix prefix(record);
  auto text = prefix.view();
  std::printf("%.*s%.*s\n", static_cast<int>(text.size()), text.data(),
              static_cast<int>(record.message.size()), record.message.data());
}
}  // namespace app
int main() {
  Platform platform;
  app::Producer producer;
  app::Consumer consumer;
  auto logger = daveos::core::make_logger(
      platform, daveos::core::SubscriberList{
                    daveos::core::Subscriber{nullptr, app::Output}});
  auto scheduler = daveos::core::make_scheduler<app::Event>(
      platform, daveos::core::ModuleList{&producer, &consumer}, logger);
  return scheduler.run() == daveos::core::Status::ok ? 0 : 1;
}
