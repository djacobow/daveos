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
        return schedule<&Producer::pulse>(std::chrono::milliseconds{1},
                                          core::Mode::repeat);
      }
      return core::Status::ok;
    }

    void pulse() {
      ++count_;
      I_("pulse %u", count_);
      scheduler().post(Pulse{count_}, this);
      if (count_ == 3) {
        // The bound timer fires in interrupt context and hands completion back
        // to a task; no global owner pointer is needed.
        if (cancel<&Producer::pulse>() != core::Status::ok ||
            timer<&Producer::expired>(std::chrono::microseconds{500}) !=
                core::Status::ok) {
          E_("could not arm the completion timer");
          scheduler().stop();
        }
      }
    }

    void complete() {
      scheduler().log_statistics();
      scheduler().stop();
    }

   private:
    void expired() {
      (void)schedule<&Producer::complete>(std::chrono::microseconds{0});
    }

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
      if (schedule<&Consumer::report>(std::chrono::microseconds{0}) !=
          core::Status::ok) {
        W_("could not schedule report");
      }
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
