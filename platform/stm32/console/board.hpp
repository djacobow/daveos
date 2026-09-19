#pragma once

#include <cinttypes>

#include "board_config.h"
#include "core/schedule/module.hpp"

namespace daveos::platform::stm32 {
  namespace core = daveos::core;
  using Platform = board::Platform;

#define DAVEOS_LED_ACTIONS(X) X(on) X(off) X(toggle)
  DAVEOS_ENUM(LedAction, std::uint8_t, DAVEOS_LED_ACTIONS)
#undef DAVEOS_LED_ACTIONS

  template <typename Event>
  class Board final : public daveos::core::Module<Board<Event>, Event> {
   public:
    Board(Platform& platform,
          void (*log_transports)(core::SchedulerInterface<Event>&))
        : platform_(platform), log_transports_(log_transports) {}

    static constexpr const char* name() { return "board"; }

    static constexpr auto commands() {
      return std::array{
          DAVEOS_COMMAND(Board, Led, "led", "Set LED on/off or toggle",
                         core::arg("led").range(1u, 3u), core::arg("action")),
          DAVEOS_COMMAND(Board, Button, "button", "Read button level"),
          DAVEOS_COMMAND(Board, Stats, "stats", "Log scheduler statistics"),
          DAVEOS_COMMAND(Board, Timer, "timer", "Start a timer",
                         core::arg("microseconds").min(1u)),
          DAVEOS_COMMAND(Board, Reset, "reset", "Reset the MCU immediately")};
    }

    core::Status init(core::InitStage stage) {
      if (stage == core::InitStage::stage1) {
        I_("DaveOS %s; type help", board::kName);
      }
      return core::Status::ok;
    }

   private:
    core::Status Timer(core::Time delay) {
      return this->template timer<&Board::TimerFired>(delay);
    }

    void TimerFired() { I_("Timer fired"); }

    core::Status Reset() { return platform_.reset(); }

    core::Status Led(std::uint8_t led, LedAction action) {
      const auto index = static_cast<std::size_t>(led - 1);
      if (action == LedAction::toggle) {
        board::ToggleLed(index);
      } else {
        board::SetLed(index, action == LedAction::on);
      }
      I_("LED %" PRIu32 " %s", static_cast<std::uint32_t>(led),
         enum_name(action));
      return core::Status::ok;
    }

    core::Status Button() {
      [[maybe_unused]] auto state = board::ReadButton();
      I_("BTN1: %s", state ? "high" : "low");
      return core::Status::ok;
    }

    core::Status Stats() {
      this->scheduler().log_statistics();
      log_transports_(this->scheduler());
      return core::Status::ok;
    }

    Platform& platform_;
    void (*log_transports_)(core::SchedulerInterface<Event>&);
  };

}  // namespace daveos::platform::stm32
