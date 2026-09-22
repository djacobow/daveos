#pragma once

#include <inttypes.h>

#include "core/schedule/module.hpp"
#include "store.h"

namespace daveos::otp {


  // Optional scheduler/console adapter. Store and its backend remain borrowed.
  // Stage1 scans storage; stage2 consumers can read the completed RAM cache.
  template <typename Event = core::NoEvent>
  class Module final : public core::Module<Module<Event>, Event> {
   public:
    explicit Module(Store& store) : store_(store) {}

    static constexpr const char* name() { return "otp"; }

    static constexpr auto commands() {
      return std::array{DAVEOS_COMMAND(Module, Serial, "serial",
                                       "serial: read; serial set <value> "
                                       "<confirmation>: strings must match"),
                        DAVEOS_COMMAND(Module, Report, "status",
                                       "OTP backend and storage health")};
    }

    core::Status init(core::InitStage stage) {
      return stage == core::InitStage::stage1 ? store_.init()
                                              : core::Status::ok;
    }

   private:
    core::Status Serial(core::CommandArguments args) {
      if (args.empty()) {
        if (!store_.ready()) {
          return core::Status::not_running;
        }
        if (const auto value = store_.serial()) {
          I_("serial: %.*s", int(value->size()), value->data());
        } else {
          I_("serial: unset");
        }
        return core::Status::ok;
      }
      if (args.size() != 3 || args[0] != "set" || args[1] != args[2]) {
        return core::Status::invalid_argument;
      }
      return store_.set_serial(args[1]);
    }

    core::Status Report() {
      [[maybe_unused]] const auto s = store_.snapshot();
      I_("backend %s, ready %s, used %" PRIu32 ", free %" PRIu32
         ", gaps %" PRIu32,
         s.backend ? s.backend : "unset", s.ready ? "yes" : "no", s.consumed,
         s.remaining, s.gaps);
      I_("invalid %" PRIu32 ", unknown %" PRIu32 ", unreadable %" PRIu32
         ", errors %" PRIu32,
         s.invalid, s.unknown, s.unreadable, s.errors);
      if (s.serial_block) {
        I_("serial block %" PRIu32 ", locked %s", *s.serial_block,
           s.serial_locked ? "yes" : "no");
      }
      if (s.errors) {
        if (s.error_block) {
          I_("last error %s during %s at block %" PRIu32, enum_name(s.error),
             enum_name(s.phase), *s.error_block);
        } else {
          I_("last error %s during %s", enum_name(s.error), enum_name(s.phase));
        }
      }
      return store_.ready() ? core::Status::ok : core::Status::not_running;
    }

    Store& store_;
  };


}  // namespace daveos::otp
