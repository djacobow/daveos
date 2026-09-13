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
  unsigned count_ = 0;
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
  static constexpr const char* levels[] = {"debug", "info", "warning", "error",
                                           "fatal"};
  std::printf("[%llu] %-7s %s/%s: %.*s\n",
              static_cast<unsigned long long>(record.timestamp),
              levels[static_cast<unsigned>(record.severity)], record.module,
              record.task, static_cast<int>(record.message.size()),
              record.message.data());
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
