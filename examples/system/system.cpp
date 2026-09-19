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

namespace core = daveos::core;

namespace app {

  enum class Event { pulse };

  class Producer final : public core::Module<Producer, Event> {
   public:
    static constexpr const char* name() { return "producer"; }

    static constexpr auto tasks() {
      return std::array{
          core::TaskDescriptor<Producer>{"pulse", &Producer::pulse},
          core::TaskDescriptor<Producer>{"complete", &Producer::complete}};
    }

    core::Status init(core::InitStage stage) {
      if (stage == core::InitStage::stage1) {
        active_ = this;
        return scheduler().schedule(*this, &Producer::pulse, 1000,
                                    core::Mode::repeat);
      }
      return core::Status::ok;
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

  class Consumer final : public core::Module<Consumer, Event> {
   public:
    static constexpr const char* name() { return "consumer"; }

    static constexpr auto tasks() {
      return std::array{
          core::TaskDescriptor<Consumer>{"report", &Consumer::report}};
    }

    void on_event(Event) { scheduler().schedule(*this, &Consumer::report, 0); }

    void report() { I_("received pulse"); }
  };

  void Output(void*, const core::LogRecord& record) {
    core::LogPrefix prefix(record);
    auto text = prefix.view();
    std::printf("%.*s%.*s\n", static_cast<int>(text.size()), text.data(),
                static_cast<int>(record.message.size()), record.message.data());
  }

  // Passive module construction; scheduler init() binds and initializes them.
  Producer producer;
  Consumer consumer;
}  // namespace app

int main() {
  Platform platform;
  auto logger = daveos::core::make_logger(
      platform, daveos::core::SubscriberList{
                    daveos::core::Subscriber{nullptr, app::Output}});
  auto scheduler = daveos::core::make_scheduler<app::Event>(
      platform, daveos::core::ModuleList{&app::producer, &app::consumer},
      logger);
  const auto status = scheduler.run();
  if (status != core::Status::ok) {
    std::fprintf(stderr, "DaveOS: %s\n", core::enum_name(status));
    return 1;
  }
  return 0;
}
