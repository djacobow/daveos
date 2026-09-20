#pragma once

#include <inttypes.h>

#include "application.h"
#include "otp/module.hpp"
#include "platform/stm32h5/otp.h"

namespace app {


  // Explicit hardware composition. No startup programming or locking.
  class HardwareOtp : public core::Module<HardwareOtp, Event> {
   private:
    daveos::platform::stm32h5::Otp backend_;
    daveos::otp::Store store_;

   public:
    daveos::otp::Module<Event> module;

    HardwareOtp(Platform& platform, bool provisioning)
        : backend_(
              &platform,
              [](void* p) { return static_cast<Platform*>(p)->now(); },
              provisioning),
          store_(backend_.driver()),
          module(store_) {}

    static constexpr const char* name() { return "otp_hw"; }

    static constexpr auto commands() {
      return std::array{DAVEOS_COMMAND(
          HardwareOtp, Inspect, "inspect",
          "Read one physical OTP block; counts and fingerprint only",
          core::arg("block").range(0u, 31u))};
    }

   private:
    core::Status Inspect(std::uint32_t index) {
      auto driver = backend_.driver();
      daveos::otp::Block block;
      auto status = driver.inspect(driver.context, index, block);
      if (status != core::Status::ok) {
        return status;
      }
      [[maybe_unused]] const auto& a = backend_.audit()[index];
      I_("block %" PRIu32 ": virgin %" PRIu32 " programmed %" PRIu32
         " unreadable %" PRIu32 " locked %s crc %08" PRIx32,
         index, a.virgin, a.programmed, a.unreadable, a.locked ? "yes" : "no",
         a.fingerprint);
      return core::Status::ok;
    }
  };


}  // namespace app
