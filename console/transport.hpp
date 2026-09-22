#pragma once

#include <inttypes.h>

#include "core/logging/log_format.hpp"
#include "module.hpp"
#include "output.hpp"

namespace daveos::console {


  // Own a serial transport and expose it as an independent command/log module.
  // Transport construction is passive; init/stop own peripheral setup/teardown.
  // The platform and any transport-owned external DMA storage must outlive us.
  // Give each instance its own name when one transport type is used twice.
  template <typename Event, typename Transport>
  class TransportModule final
      : public Module<TransportModule<Event, Transport>, Event> {
   public:
    template <typename Platform>
    explicit TransportModule(Platform& platform, const char* name = nullptr)
        : Module<TransportModule<Event, Transport>, Event>(name),
          transport_(platform) {}

    static constexpr const char* name() { return Transport::name(); }

    core::Status init(core::InitStage stage) {
      if (stage == core::InitStage::stage1 && !transport_.init()) {
        return core::Status::initialization_failed;
      }
      return Module<TransportModule<Event, Transport>, Event>::init(stage);
    }

    void stop() { transport_.stop(); }

    bool poll_line(Line& line) { return transport_.poll_line(line); }

    std::uint32_t take_dropped() { return transport_.take_dropped(); }

    void output(const core::LogRecord& record) { transport_.output(record); }

    void log_statistics(
        [[maybe_unused]] core::SchedulerInterface<Event>& scheduler) {
      [[maybe_unused]] const auto counters = transport_.counters();
      DAVEOS_LOG(scheduler, core::Level::info,
                 "%s: %s bytes, %" PRIu32 " transfers, %" PRIu32
                 " dropped frames, %" PRIu32 " errors",
                 Transport::statistics_label(),
                 core::LogUnsigned(counters.sent_bytes).c_str(),
                 counters.transfers, counters.dropped_frames, counters.errors);
    }

   private:
    Transport transport_;
  };


}  // namespace daveos::console
