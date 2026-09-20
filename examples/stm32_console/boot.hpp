#pragma once

#include "application.h"
#include "boot/control.h"
#include "core/schedule/module.hpp"
#include "layout.h"
#include "platform/stm32h5/reliability.h"
#include "stm32h563xx.h"

namespace app {


  // Explicit confirmation for bring-up. The application will later decide
  // when its health criteria permit calling the same Control API.
  class Boot : public core::Module<Boot, Event> {
   public:
    explicit Boot(Platform& platform)
        : flash_(&platform,
                 [](void* p) { return static_cast<Platform*>(p)->now(); }),
          control_(flash_.driver(), daveos::boot::kLayout) {}

    static constexpr const char* name() { return "boot"; }

    static constexpr auto commands() {
      return std::array{DAVEOS_COMMAND(Boot, Status, "status",
                                       "Show executing slot and image state"),
                        DAVEOS_COMMAND(Boot, Confirm, "confirm",
                                       "Explicitly confirm this trial image")};
    }

    core::Status Status() {
      daveos::boot::Snapshot snapshot;
      auto result = control_.status(snapshot);
      if (result == core::Status::ok) {
        auto slot = Slot();
        if (slot >= 2) {
          return core::Status::unsupported;
        }
        const auto state = snapshot.images[slot].state;
        I_("Slot %c: %s", 'A' + static_cast<int>(slot),
           state == daveos::boot::ImageState::confirmed ? "confirmed"
           : state == daveos::boot::ImageState::trial   ? "trial"
                                                        : "not eligible");
      }
      return result;
    }

    core::Status Confirm() {
      auto result = control_.confirm_image(Slot());
      if (result == core::Status::ok) {
        I_("Image confirmed");
      }
      return result;
    }

   private:
    static std::uint32_t Slot() {
      for (std::uint32_t slot = 0; slot < 2; ++slot) {
        if (SCB->VTOR == daveos::boot::kLayout.slots[slot]) {
          return slot;
        }
      }
      return 2;
    }

    daveos::platform::stm32h5::Flash flash_;
    daveos::boot::Control control_;
  };


}  // namespace app
