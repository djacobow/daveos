#include "service.h"

#include <algorithm>

extern "C" {
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/prot/dhcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"
}

namespace daveos::net {


const char* state_name(State state) {
  switch (state) {
    case State::stopped:
      return "stopped";
    case State::link_down:
      return "link down";
    case State::addressing:
      return "waiting for DHCP";
    case State::ready:
      return "ready";
    case State::hardware_fault:
      return "hardware fault";
  }
  return "unknown";
}
const char* link_name(Link link) {
  switch (link) {
    case Link::down:
      return "down";
    case Link::half10:
      return "10M half duplex";
    case Link::full10:
      return "10M full duplex";
    case Link::half100:
      return "100M half duplex";
    case Link::full100:
      return "100M full duplex";
    case Link::fault:
      return "fault";
  }
  return "unknown";
}
struct NetworkState {
  Service* owner = nullptr;
  netif interface {};
  bool initialized = false;
  std::uint32_t random = 1;
  std::array<std::uint8_t, 1536> frame{};
  static err_t Transmit(netif* interface, pbuf* packet) {
    auto& self = *static_cast<NetworkState*>(interface->state);
    auto& service = *self.owner;
    if (packet->tot_len > self.frame.size() ||
        pbuf_copy_partial(packet, self.frame.data(), packet->tot_len, 0) !=
            packet->tot_len ||
        !service.driver_.transmit(service.driver_.context,
                                  {self.frame.data(), packet->tot_len})) {
      ++service.snapshot_.dropped_tx;
      return ERR_MEM;
    }
    ++service.snapshot_.tx;
    return ERR_OK;
  }
  static err_t Init(netif* interface) {
    auto& self = *static_cast<NetworkState*>(interface->state);
    interface->name[0] = 'e';
    interface->name[1] = 'n';
    interface->hwaddr_len = 6;
    std::copy(self.owner->config_.mac.begin(), self.owner->config_.mac.end(),
              interface->hwaddr);
    interface->mtu = 1500;
    interface->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
    interface->output = etharp_output;
    interface->linkoutput = Transmit;
    interface->hostname = "daveos";
    return ERR_OK;
  }
  std::uint32_t Now() const {
    return owner ? owner->clock_.milliseconds(owner->clock_.context) : 0;
  }
};
static NetworkState stack;
static ip4_addr_t Address(const Ipv4& bytes) {
  ip4_addr_t result;
  IP4_ADDR(&result, bytes[0], bytes[1], bytes[2], bytes[3]);
  return result;
}
static Ipv4 Bytes(const ip4_addr_t* value) {
  return {ip4_addr1(value), ip4_addr2(value), ip4_addr3(value),
          ip4_addr4(value)};
}
Service::Service(Driver driver, Clock clock, const Config& config)
    : driver_(driver), clock_(clock), config_(config) {}
Service::~Service() { stop(); }
bool Service::init(const Config& config) {
  if (attempted_) return false;
  config_ = config;
  return init();
}
bool Service::init() {
  if (attempted_) return false;
  attempted_ = true;
  snapshot_.mac = config_.mac;
  if (stack.owner || (config_.mac[0] & 1) ||
      std::all_of(config_.mac.begin(), config_.mac.end(),
                  [](auto b) { return b == 0; }) ||
      !driver_.init(driver_.context, config_.mac)) {
    snapshot_.state = State::hardware_fault;
    return false;
  }
  stack.owner = this;
  for (auto b : config_.mac) stack.random = stack.random * 33 + b;
  if (!stack.initialized) {
    lwip_init();
    stack.initialized = true;
  } else {
    sys_restart_timeouts();
  }
  stack.interface = {};
  const auto address = Address(config_.dhcp ? Ipv4{} : config_.address);
  const auto mask = Address(config_.dhcp ? Ipv4{} : config_.netmask);
  const auto gateway = Address(config_.dhcp ? Ipv4{} : config_.gateway);
  netif_add(&stack.interface, &address, &mask, &gateway, &stack,
            NetworkState::Init, ethernet_input);
  netif_set_default(&stack.interface);
  netif_set_up(&stack.interface);
  active_ = true;
  snapshot_.state = State::link_down;
  last_link_check_ = stack.Now() - 250;
  return true;
}
void Service::CheckLink() {
  const auto now = stack.Now();
  if (now - last_link_check_ < 250) return;
  last_link_check_ = now;
  const auto link = driver_.link(driver_.context);
  if (link != snapshot_.link) {
    snapshot_.link = link;
    if (link == Link::down || link == Link::fault) {
      netif_set_link_down(&stack.interface);
      if (config_.dhcp) {
        dhcp_stop(&stack.interface);
        ip4_addr_t zero{};
        netif_set_addr(&stack.interface, &zero, &zero, &zero);
      }
    } else {
      netif_set_link_up(&stack.interface);
    }
  }
  if (config_.dhcp && netif_is_link_up(&stack.interface) &&
      (!netif_dhcp_data(&stack.interface) ||
       netif_dhcp_data(&stack.interface)->state == DHCP_STATE_OFF)) {
    if (dhcp_start(&stack.interface) != ERR_OK) ++stack_errors_;
  }
}
void Service::poll() {
  if (!active_) return;
  driver_.poll(driver_.context);
  CheckLink();
  for (unsigned i = 0; i < 4; ++i) {
    const auto size = driver_.receive(driver_.context, stack.frame);
    if (!size) break;
    if (size > stack.frame.size() || !netif_is_link_up(&stack.interface)) {
      ++snapshot_.dropped_rx;
      continue;
    }
    auto* packet = pbuf_alloc(PBUF_RAW, static_cast<u16_t>(size), PBUF_POOL);
    if (!packet) {
      ++snapshot_.dropped_rx;
      continue;
    }
    pbuf_take(packet, stack.frame.data(), static_cast<u16_t>(size));
    ++snapshot_.rx;
    if (ethernet_input(packet, &stack.interface) != ERR_OK) {
      pbuf_free(packet);
      ++snapshot_.dropped_rx;
    }
  }
  sys_check_timeouts();
  snapshot_.address = Bytes(netif_ip4_addr(&stack.interface));
  snapshot_.netmask = Bytes(netif_ip4_netmask(&stack.interface));
  snapshot_.gateway = Bytes(netif_ip4_gw(&stack.interface));
  snapshot_.errors = driver_.errors(driver_.context) + stack_errors_;
  snapshot_.state = snapshot_.link == Link::fault ? State::hardware_fault
                    : !netif_is_link_up(&stack.interface) ? State::link_down
                    : snapshot_.address == Ipv4{}         ? State::addressing
                                                          : State::ready;
}
void Service::stop() {
  if (!active_) return;
  dhcp_stop(&stack.interface);
  dhcp_cleanup(&stack.interface);
  netif_set_down(&stack.interface);
  netif_remove(&stack.interface);
  driver_.stop(driver_.context);
  stack.owner = nullptr;
  active_ = false;
  snapshot_.state = State::stopped;
}


}  // namespace daveos::net
extern "C" std::uint32_t sys_now() { return daveos::net::stack.Now(); }
extern "C" std::uint32_t daveos_net_random() {
  auto& x = daveos::net::stack.random;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return x;
}
