#pragma once

#include "boot/flash.h"
#include "util/fault.h"
#include "watchdog/driver.h"

namespace daveos::platform::stm32h5 {


  // Caller-owned flash driver. The injected clock is valid before scheduling;
  // used on the scheduler thread. All instances share peripheral ownership
  // until the initiating instance polls completion. Other instances return
  // busy without acknowledging its flags. Constructors do not touch hardware.
  class Flash {
   public:
    Flash(void* clock_context, core::Time (*clock)(void*))
        : clock_context_(clock_context), clock_(clock) {}

    Flash(const Flash&) = delete;
    Flash& operator=(const Flash&) = delete;

    boot::Flash driver();

   private:
    friend class Otp;
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

  // Enables early warning as well as reset. Link iwdg_handler.S for retained
  // frame capture and reserve the same fault RAM/stack as the H563 board.
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
  // Copy current image identity and remember whether this boot recorded a
  // watchdog health failure, so early-warning capture can enrich its frame.
  void record_failure(util::fault::Data data);
  [[noreturn]] void reset();


}  // namespace daveos::platform::stm32h5

extern "C" void DaveOS_FaultCapture(const std::uint32_t* frame,
                                    std::uint32_t exc_return,
                                    std::uint32_t kind);
