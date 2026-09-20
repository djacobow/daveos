#pragma once

#include "util/fault.h"
#include "watchdog/driver.h"

namespace daveos::platform::stm32h7 {


  // M7 owns IWDG1; M4 remains asleep. H755 has no IWDG early-warning IRQ.
  // Constructors are passive. A health failure can be retained before reset,
  // but an arbitrary hang cannot supply a pre-reset exception frame.
  class Watchdog {
   public:
    watchdog::Driver driver();

   private:
    core::Status Start(core::Time timeout);
    core::Status Feed();
    bool started_ = false;
  };

  bool prepare_health(const util::Version& version);
  [[noreturn]] void initialization_failed();
  // First 2 KiB of DTCM are reserved for the record and emergency stack.
  util::fault::Record& retained_fault();
  void set_fault_identity(const util::Version& version,
                          std::uint64_t installation);
  void record_failure(util::fault::Data data);
  [[noreturn]] void reset();


}  // namespace daveos::platform::stm32h7

extern "C" void DaveOS_FaultCapture(const std::uint32_t* frame,
                                    std::uint32_t exc_return,
                                    std::uint32_t kind);
