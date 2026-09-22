#pragma once

#include "net/update.h"
#include "ota.hpp"

namespace app {


  // Optional network transport; SD updating does not depend on lwIP.
  class OtaNetwork {
   public:
    template <bool Files>
    OtaNetwork(Platform& platform, daveos::net::Service& network,
               Ota<Files>& ota)
        : platform_(platform),
          server_(network, ota.engine(),
                  {this, [](void* p) {
                     auto& self = *static_cast<OtaNetwork*>(p);
                     self.reboot_at_ = self.platform_.now() + kRebootDelay;
                     self.reboot_requested_ = true;
                     return core::Status::ok;
                   }}) {
      ota.module.transport(
          this, [](void* p) { static_cast<OtaNetwork*>(p)->Poll(); });
    }

    void stop() { server_.stop(); }

   private:
    static constexpr core::Time kRebootDelay = 250000;

    void Poll() {
      server_.poll();
      if (reboot_requested_ && platform_.now() >= reboot_at_) {
        platform_.reset();
      }
    }

    Platform& platform_;
    daveos::net::UpdateServer server_;
    bool reboot_requested_ = false;
    core::Time reboot_at_ = 0;
  };


}  // namespace app
