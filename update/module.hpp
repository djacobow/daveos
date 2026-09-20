#pragma once

#include <cinttypes>

#include "core/schedule/module.hpp"
#include "protocol.h"

namespace daveos::update {


  // Optional scheduler/command adapter. The engine and transport remain usable
  // without DaveOS. Transport polling is supplied after passive construction.
  template <typename Event = core::NoEvent>
  class Module : public core::Module<Module<Event>, Event> {
   public:
    explicit Module(Engine& engine) : engine_(engine) {}

    static constexpr const char* name() { return "ota"; }

    static constexpr auto tasks() {
      return std::array{
          DAVEOS_PERIODIC(Module, Poll, std::chrono::milliseconds{1})};
    }

    static constexpr auto commands() {
      return std::array{
          DAVEOS_COMMAND(Module, Enable, "enable",
                         "Enable firmware uploads until reset"),
          DAVEOS_COMMAND(Module, Disable, "disable",
                         "Disable uploads and abort incomplete installation"),
          DAVEOS_COMMAND(Module, Report, "status",
                         "Report firmware update progress")};
    }

    void transport(void* context, void (*poll)(void*)) {
      context_ = context;
      poll_ = poll;
    }

    void setup(void* context, core::Status (*initialize)(void*)) {
      setup_context_ = context;
      initialize_ = initialize;
    }

    core::Status init(core::InitStage stage) {
      return stage == core::InitStage::stage1 && initialize_
                 ? initialize_(setup_context_)
                 : core::Status::ok;
    }

   private:
    void Poll() {
      engine_.tick();
      if (poll_) {
        poll_(context_);
      }
    }

    core::Status Enable() {
      engine_.enable(true);
      I_("OTA enabled");
      return core::Status::ok;
    }

    core::Status Disable() {
      engine_.enable(false);
      I_("OTA disabled");
      return core::Status::ok;
    }

    core::Status Report() {
      I_("OTA %s: %s; received %" PRIu32 "/%" PRIu32,
         state_name(engine_.state()), enum_name(engine_.status()),
         engine_.next_offset(), engine_.total());
      return core::Status::ok;
    }

    Engine& engine_;
    void* context_ = nullptr;
    void (*poll_)(void*) = nullptr;
    void* setup_context_ = nullptr;
    core::Status (*initialize_)(void*) = nullptr;
  };


}  // namespace daveos::update
