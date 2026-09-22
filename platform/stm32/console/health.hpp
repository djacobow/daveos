#pragma once

#include "board_config.h"
#include "watchdog/health.hpp"

namespace daveos::platform::stm32 {


  // Nucleo reliability services (IWDG, retained fault record, hardware CRC)
  // for watchdog::HealthModule. board::reliability names the H5 or H7
  // implementation selected by board_config.h.
  struct HealthHardware {
    using Watchdog = board::reliability::Watchdog;

    static bool prepare_health(const util::Version& version) {
      return board::reliability::prepare_health(version);
    }

    [[noreturn]] static void initialization_failed() {
      board::reliability::initialization_failed();
    }

    static util::fault::Record& retained_fault() {
      return board::reliability::retained_fault();
    }

    static void record_failure(util::fault::Data data) {
      board::reliability::record_failure(data);
    }

    static util::crc32::Backend crc32_backend() {
      return board::reliability::crc32_backend();
    }
  };

  template <typename Event>
  using Health = watchdog::HealthModule<Event, board::Platform, HealthHardware>;


}  // namespace daveos::platform::stm32
