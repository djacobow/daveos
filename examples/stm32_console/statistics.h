#pragma once

#include <inttypes.h>

#include "application.h"
#include "console/output.hpp"
#include "core/logging/log_format.hpp"

namespace app {
  // Called by board stats; preserve the requesting command's logging context.
  inline void LogTx([[maybe_unused]] core::SchedulerInterface<Event>& scheduler,
                    [[maybe_unused]] const char* name,
                    [[maybe_unused]] daveos::console::TxCounters counters) {
    DAVEOS_LOG(scheduler, core::Level::info,
               "%s: %s bytes, %" PRIu32 " transfers, %" PRIu32
               " dropped frames, %" PRIu32 " errors",
               name, core::LogUnsigned(counters.sent_bytes).c_str(),
               counters.transfers, counters.dropped_frames, counters.errors);
  }
}  // namespace app
