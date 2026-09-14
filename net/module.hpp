#pragma once

#include <inttypes.h>

#include "core/schedule/module.hpp"
#include "service.h"

namespace daveos::net {


// Optional DaveOS adapter. The service, its clock, and driver remain
// application owned. Hardware faults never fail initialization of unrelated
// modules.
template <typename Event>
class Module final : public core::Module<Module<Event>, Event> {
 public:
  explicit Module(Service& service) : service_(service) {}
  static constexpr const char* name() { return "net"; }
  static constexpr auto tasks() {
    return std::array{core::TaskDescriptor<Module>{"poll", &Module::Poll}};
  }
  static constexpr auto commands() {
    return std::array{DAVEOS_COMMAND(Module, "status", Status,
                                     "Ethernet link, IPv4, and counters")};
  }
  core::Status init(core::InitStage stage) {
    if (stage != core::InitStage::stage1) return core::Status::ok;
    if (!service_.init()) {
      E_("Ethernet initialization failed; reset to retry");
      return core::Status::ok;
    }
    return this->scheduler().schedule(*this, &Module::Poll, 1000,
                                      core::Mode::repeat);
  }

 private:
  void Poll() {
    service_.poll();
    const auto current = service_.snapshot();
    if (current.state != previous_.state || current.link != previous_.link ||
        current.address != previous_.address)
      Report(current);
    previous_ = current;
  }
  core::Status Status(core::CommandArguments args) {
    if (!args.empty()) return core::Status::invalid_argument;
    Report(service_.snapshot());
    return core::Status::ok;
  }
  void Report([[maybe_unused]] const Snapshot& s) {
    I_("%s, link %s, IP %" PRIu32 ".%" PRIu32 ".%" PRIu32 ".%" PRIu32,
       state_name(s.state), link_name(s.link), std::uint32_t(s.address[0]),
       std::uint32_t(s.address[1]), std::uint32_t(s.address[2]),
       std::uint32_t(s.address[3]));
    I_("Mask %" PRIu32 ".%" PRIu32 ".%" PRIu32 ".%" PRIu32 " gateway %" PRIu32
       ".%" PRIu32 ".%" PRIu32 ".%" PRIu32,
       std::uint32_t(s.netmask[0]), std::uint32_t(s.netmask[1]),
       std::uint32_t(s.netmask[2]), std::uint32_t(s.netmask[3]),
       std::uint32_t(s.gateway[0]), std::uint32_t(s.gateway[1]),
       std::uint32_t(s.gateway[2]), std::uint32_t(s.gateway[3]));
    I_("MAC %02" PRIx32 ":%02" PRIx32 ":%02" PRIx32 ":%02" PRIx32 ":%02" PRIx32
       ":%02" PRIx32,
       std::uint32_t(s.mac[0]), std::uint32_t(s.mac[1]),
       std::uint32_t(s.mac[2]), std::uint32_t(s.mac[3]),
       std::uint32_t(s.mac[4]), std::uint32_t(s.mac[5]));
    I_("RX %" PRIu32 " TX %" PRIu32 " dropped RX/TX %" PRIu32 "/%" PRIu32
       " errors %" PRIu32,
       s.rx, s.tx, s.dropped_rx, s.dropped_tx, s.errors);
  }
  Service& service_;
  Snapshot previous_{};
};


}  // namespace daveos::net
