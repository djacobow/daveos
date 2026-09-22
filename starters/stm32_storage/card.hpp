#pragma once

#include <cinttypes>

#include "core/schedule/module.hpp"
#include "platform/stm32/console/sd_card.hpp"

namespace app {


  namespace core = daveos::core;
  using Event = daveos::core::NoEvent;

  // Owns the Nucleo SD card and probes it once dispatch starts. Mount it with
  // `fs mount`; `card probe` re-initializes after removal or an error.
  class Card : public core::Module<Card> {
   public:
    explicit Card(board::Platform& platform) : card_(platform, false) {}

    static constexpr const char* name() { return "card"; }

    static constexpr auto tasks() {
      return std::array{
          DAVEOS_PERIODIC(Card, Tick, std::chrono::milliseconds{1})};
    }

    static constexpr auto commands() {
      return std::array{
          DAVEOS_COMMAND(Card, Probe, "probe", "Initialize the SD card")};
    }

    core::Status init(core::InitStage stage) {
      if (stage == core::InitStage::stage1) {
        return card_.init(scheduler()) == daveos::hal::Status::ok
                   ? core::Status::ok
                   : core::Status::initialization_failed;
      }
      return Probe();
    }

    core::Status Probe() {
      reported_ = false;
      return card_.session().request() == daveos::hal::Status::ok
                 ? core::Status::ok
                 : core::Status::busy;
    }

    void interrupt() { card_.interrupt(); }

    daveos::storage::BlockDevice block_device() { return card_.block_device(); }

   private:
    void Tick() {
      card_.tick();
      auto& session = card_.session();
      if (!reported_ && !session.running()) {
        reported_ = true;
        if (session.ready()) {
          I_("SD card ready: %" PRIu32 " MiB",
             static_cast<std::uint32_t>(session.card().sectors / 2048));
        } else {
          E_("SD card not ready: %s (CMD%" PRIu32 ")", session.error(),
             session.command());
        }
      }
    }

    daveos::platform::stm32::SdCard<Event> card_;
    bool reported_ = true;
  };


}  // namespace app
