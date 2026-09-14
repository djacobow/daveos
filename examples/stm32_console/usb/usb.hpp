#pragma once

#include <inttypes.h>

#include "../input.hpp"
#include "../output.hpp"
#include "board_config.h"
#include "core/command/source.hpp"
#include "core/logging/log.hpp"
#include "core/schedule/module.hpp"

namespace board {


// Shared STM32 USB console. All entry points except middleware callbacks run
// on the scheduler thread. No output is retained while unconfigured/DTR-low.
bool InitUsb(Platform& platform);
void StopUsb();
bool PollUsb(app::Line& line);
void OutputUsb(const daveos::core::LogRecord& record);
app::TxCounters UsbCounters();
std::uint32_t UsbDroppedInput();

// Independent USB command source and log subscriber. The application registers
// command_source() and subscriber(), and includes this module in ModuleList.
template <typename Event>
class UsbConsole final : public daveos::core::Module<UsbConsole<Event>, Event> {
 public:
  static constexpr const char* name() { return "usb"; }
  static constexpr auto tasks() {
    return std::array{
        daveos::core::TaskDescriptor<UsbConsole>{"input", &UsbConsole::Poll}};
  }
  daveos::core::CommandSource& command_source() { return source_; }
  daveos::core::Status init(daveos::core::InitStage stage) {
    if (stage != daveos::core::InitStage::stage1)
      return daveos::core::Status::ok;
    return this->scheduler().schedule(*this, &UsbConsole::Poll, 1000,
                                      daveos::core::Mode::repeat);
  }
  daveos::core::Subscriber subscriber() {
    return {this, [](void*, const daveos::core::LogRecord& record) {
              OutputUsb(record);
            }};
  }

 private:
  void Poll() {
    [[maybe_unused]] auto dropped = UsbDroppedInput();
    if (dropped) W_("Dropped %" PRIu32 " USB input lines/errors", dropped);
    app::Line line;
    if (!PollUsb(line)) return;
    if (line.size) I_("> %.*s", static_cast<int>(line.size), line.bytes.data());
    source_.dispatch(line.view());
  }
  daveos::core::CommandSource source_;
};


}  // namespace board
