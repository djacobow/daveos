#pragma once

#include <charconv>

#include "application.h"

namespace app {
  class Board final : public daveos::core::Module<Board, Event> {
   public:
    Board(Platform& platform,
          void (*log_transports)(core::SchedulerInterface<Event>&))
        : platform_(platform), log_transports_(log_transports) {}

    static constexpr const char* name() { return "board"; }

    static constexpr auto commands() {
      return std::array{
          DAVEOS_COMMAND(Board, "led", Led, "led <1|2|3> <on|off|toggle>"),
          DAVEOS_COMMAND(Board, "button", Button, "Read button level"),
          DAVEOS_COMMAND(Board, "stats", Stats, "Log scheduler statistics"),
          DAVEOS_COMMAND(Board, "timer", Timer, "timer <microseconds>"),
          DAVEOS_COMMAND(Board, "reset", Reset, "Reset the MCU immediately")};
    }

    core::Status init(core::InitStage stage) {
      if (stage == core::InitStage::stage1) {
        I_("DaveOS %s; type help", board::kName);
      }
      return core::Status::ok;
    }

   private:
    core::Status Timer(core::CommandArguments args) {
      if (args.size() != 1 || args[0].empty()) {
        return core::Status::invalid_argument;
      }
      core::Time delay = 0;
      const auto text = args[0];
      const auto end = text.data() + text.size();
      const auto parsed = std::from_chars(text.data(), end, delay);
      if (parsed.ec != std::errc{} || parsed.ptr != end || !delay) {
        return core::Status::invalid_argument;
      }
      return timer<&Board::TimerFired>(delay);
    }

    void TimerFired() { I_("Timer fired"); }

    core::Status Reset(core::CommandArguments args) {
      if (!args.empty()) {
        return core::Status::invalid_argument;
      }
      return platform_.reset();
    }

    core::Status Led(core::CommandArguments args) {
      if (args.size() != 2 || args[0].size() != 1 || args[0][0] < '1' ||
          args[0][0] > '3') {
        return core::Status::invalid_argument;
      }
      auto index = static_cast<std::size_t>(args[0][0] - '1');
      if (args[1] == "toggle") {
        board::ToggleLed(index);
      } else if (args[1] == "on" || args[1] == "off") {
        board::SetLed(index, args[1] == "on");
      } else {
        return core::Status::invalid_argument;
      }
      I_("LED %c %.*s", args[0][0], static_cast<int>(args[1].size()),
         args[1].data());
      return core::Status::ok;
    }

    core::Status Button(core::CommandArguments args) {
      if (!args.empty()) {
        return core::Status::invalid_argument;
      }
      [[maybe_unused]] auto state = board::ReadButton();
      I_("BTN1: %s", state ? "high" : "low");
      return core::Status::ok;
    }

    core::Status Stats(core::CommandArguments args) {
      if (!args.empty()) {
        return core::Status::invalid_argument;
      }
      scheduler().log_statistics();
      log_transports_(scheduler());
      return core::Status::ok;
    }

    Platform& platform_;
    void (*log_transports_)(core::SchedulerInterface<Event>&);
  };

}  // namespace app
