#include <cinttypes>
#include <cstdint>
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

  struct Pulse {
    std::uint32_t sequence = 0;
  };

  using Event = std::variant<Pulse>;

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
      scheduler().post(Pulse{count_}, this);
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

    static constexpr auto events() {
      return std::tuple{DAVEOS_EVENT(Consumer, OnPulse)};
    }

    void OnPulse(const Pulse& pulse) {
      sequence_ = pulse.sequence;
      scheduler().schedule(*this, &Consumer::report, 0);
    }

    void report() { I_("received pulse %" PRIu32, sequence_); }

   private:
    std::uint32_t sequence_ = 0;
  };

  // Passive module construction; scheduler init() binds and initializes them.
  Producer producer;
  Consumer consumer;
}  // namespace app

int main() {
  Platform platform;
  auto logger = daveos::core::make_logger(
      platform, daveos::core::SubscriberList{
                    daveos::platform::host::stdout_subscriber()});
  auto scheduler = daveos::core::make_scheduler<app::Event>(
      platform, daveos::core::ModuleList{&app::producer, &app::consumer},
      logger);
  return daveos::platform::host::run(scheduler);
}
