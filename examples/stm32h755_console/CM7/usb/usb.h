#pragma once

#include "../../../stm32_console/input.hpp"
#include "../../../stm32_console/output.hpp"
#include "board_config.h"
#include "core/logging/log.hpp"

namespace board {


// H755-only second console. All entry points except middleware callbacks run
// on the scheduler thread. No output is retained while unconfigured/DTR-low.
bool InitUsb(Platform& platform);
void StopUsb();
bool PollUsb(app::Line& line);
void OutputUsb(const daveos::core::LogRecord& record);
app::TxCounters UsbCounters();
std::uint32_t UsbDroppedInput();


}  // namespace board
