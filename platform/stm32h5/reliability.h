#pragma once

#include "boot/flash.h"
#include "util/fault.h"
#include "watchdog/driver.h"

namespace daveos::platform::stm32h5 {


  // Caller-owned flash driver. The injected clock is valid before scheduling;
  // one owner serializes operations. Constructors do not touch hardware.
  class Flash {
   public:
    Flash(void* clock_context, core::Time (*clock)(void*))
        : clock_context_(clock_context), clock_(clock) {}

    boot::Flash driver();

   private:
    core::Status Read(std::uint32_t address, std::span<std::byte> bytes);
    core::Status Erase(std::uint32_t address);
    core::Status Program(std::uint32_t address,
                         std::span<const std::byte> bytes);
    core::Status Poll();
    core::Status Unlock();
    void* clock_context_;
    core::Time (*clock_)(void*);
    bool pending_ = false;
  };

  class Watchdog {
   public:
    watchdog::Driver driver();

   private:
    core::Status Start(core::Time timeout);
    core::Status Feed();
    bool started_ = false;
  };

  util::fault::Record& retained_fault();
  // Explicit validation reads can recover from flash ECC NMI and return an
  // error. Other NMIs follow the fault/reset path.
  bool handle_flash_ecc();
  void set_fault_identity(const util::Version& version,
                          std::uint64_t installation);
  [[noreturn]] void reset();


}  // namespace daveos::platform::stm32h5

extern "C" void DaveOS_FaultCapture(const std::uint32_t* frame,
                                    std::uint32_t exc_return,
                                    std::uint32_t kind);
