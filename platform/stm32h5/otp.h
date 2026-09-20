#pragma once

#include "otp/driver.h"
#include "otp_ecc.h"
#include "reliability.h"

namespace daveos::platform::stm32h5 {


  // Scheduler-thread-only real OTP adapter. Default is read-only, including
  // lock operations. The board supplies an existing noncacheable/XN MPU region
  // covering OTP; write access changes only its AP field and restores it.
  class Otp {
   public:
    struct Audit {
      std::uint32_t virgin = 0, programmed = 0, unreadable = 0;
      std::uint32_t fingerprint = 0;
      bool locked = false;
    };

    Otp(void* clock_context, core::Time (*clock)(void*),
        bool allow_provisioning = false, std::uint32_t mpu_region = 0)
        : flash_(clock_context, clock),
          allow_provisioning_(allow_provisioning),
          mpu_region_(mpu_region) {}

    Otp(const Otp&) = delete;
    Otp& operator=(const Otp&) = delete;
    otp::Driver driver();

    const std::array<Audit, otp::kBlockCount>& audit() const { return audit_; }

   private:
    core::Status Init();
    core::Status Inspect(std::uint32_t block, otp::Block& out);
    otp::ProgramResult Program(std::uint32_t block, const otp::Record& record);
    core::Status Lock(std::uint32_t block);
    core::Status Wait();
    bool Mapping() const;
    std::uint32_t Permissions(std::uint32_t access);
    Flash flash_;
    bool allow_provisioning_, initialized_ = false, timed_out_ = false;
    std::uint32_t mpu_region_;
    std::array<bool, otp::kBlockCount> attempted_{};
    std::array<Audit, otp::kBlockCount> audit_{};
  };


}  // namespace daveos::platform::stm32h5
