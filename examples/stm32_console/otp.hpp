#pragma once

#include "application.h"
#include "layout.h"
#include "otp/flash.h"
#include "otp/module.hpp"
#include "platform/stm32h5/reliability.h"

namespace app {


  // Explicit flash-emulator composition. No hardware OTP/fuse accesses.
  class Otp {
   private:
    daveos::platform::stm32h5::Flash flash_;
    daveos::otp::FlashOtp backend_;
    daveos::otp::Store store_;

   public:
    daveos::otp::Module<Event> module;

    explicit Otp(Platform& platform)
        : flash_(&platform,
                 [](void* p) { return static_cast<Platform*>(p)->now(); }),
          backend_(flash_.driver(), daveos::boot::kOtpEmulatorBase),
          store_(backend_.driver()),
          module(store_) {
      static_assert(daveos::otp::FlashOtp::kSize ==
                    daveos::boot::kOtpEmulatorSize);
    }
  };


}  // namespace app
