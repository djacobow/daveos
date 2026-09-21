#pragma once

#include "application.h"
#include "board_config.h"
#include "layout.h"
#include "net/update.h"
#include "update/module.hpp"

namespace app {


  // Application composition only. Constructors store references; stage1 binds
  // the executing slot. Uploads remain disabled until the application enables
  // them.
  class Ota {
   private:
    static constexpr core::Time kRebootDelay = 250000;
    Platform& platform_;
    board::reliability::Flash flash_;
    daveos::update::Engine engine_;
    daveos::net::UpdateServer server_;
    bool reboot_requested_ = false;
    core::Time reboot_at_ = 0;

   public:
    daveos::update::Module<Event> module;

    Ota(Platform& platform, daveos::net::Service& network)
        : platform_(platform),
          flash_(&platform,
                 [](void* p) { return static_cast<Platform*>(p)->now(); }),
          engine_(flash_.driver(), daveos::boot::kLayout, 2),
          server_(network, engine_,
                  {this,
                   [](void* p) {
                     auto& self = *static_cast<Ota*>(p);
                     self.reboot_at_ = self.platform_.now() + kRebootDelay;
                     self.reboot_requested_ = true;
                     return core::Status::ok;
                   }}),
          module(engine_) {
      module.setup(this, [](void* p) {
        auto& self = *static_cast<Ota*>(p);
        for (std::uint32_t slot = 0; slot < 2; ++slot) {
          if (SCB->VTOR == daveos::boot::kLayout.slots[slot]) {
            return self.engine_.running_slot(slot);
          }
        }
        return core::Status::incompatible;
      });
      module.transport(this, [](void* p) {
        auto& self = *static_cast<Ota*>(p);
        self.server_.poll();
        if (self.reboot_requested_ && self.platform_.now() >= self.reboot_at_) {
          self.platform_.reset();
        }
      });
    }

    void stop() { server_.stop(); }
  };


}  // namespace app
