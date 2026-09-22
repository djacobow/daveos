#pragma once

#include "application.h"
#include "board_config.h"
#include "layout.h"
#include "update/module.hpp"

namespace app {


  // Application composition only. Constructors store references; stage1 binds
  // the executing slot. Uploads remain disabled until the application enables
  // them.
  template <bool Files = false>
  class Ota {
   private:
    board::reliability::Flash flash_;
    daveos::update::Engine engine_;

   public:
    daveos::update::Module<Event, Files> module;

    Ota(Platform& platform, daveos::update::FileSource source = {})
        : flash_(&platform,
                 [](void* p) { return static_cast<Platform*>(p)->now(); }),
          engine_(flash_.driver(), daveos::boot::kLayout, 2),
          module(engine_, source) {
      module.setup(this, [](void* p) {
        auto& self = *static_cast<Ota*>(p);
        for (std::uint32_t slot = 0; slot < 2; ++slot) {
          if (SCB->VTOR == daveos::boot::kLayout.slots[slot]) {
            return self.engine_.running_slot(slot);
          }
        }
        return core::Status::incompatible;
      });
    }

    daveos::update::Engine& engine() { return engine_; }
  };


}  // namespace app
