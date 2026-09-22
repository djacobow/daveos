#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace daveos::net {


  using Ipv4 = std::array<std::uint8_t, 4>;
  using Mac = std::array<std::uint8_t, 6>;
  enum class Link { down, half10, full10, half100, full100, fault };
  enum class State { stopped, link_down, addressing, ready, hardware_fault };
  const char* state_name(State state);
  const char* link_name(Link link);

  struct Config {
    Mac mac{0x02, 0, 0, 0, 0, 1};
    bool dhcp = true;
    Ipv4 address{192, 168, 50, 2};
    Ipv4 netmask{255, 255, 255, 0};
    Ipv4 gateway{};
  };

  struct Snapshot {
    State state = State::stopped;
    Link link = Link::down;
    Ipv4 address{}, netmask{}, gateway{};
    Mac mac{};
    std::uint32_t rx = 0, tx = 0, dropped_rx = 0, dropped_tx = 0, errors = 0;
  };

  // Borrowed driver and clock. All methods run in one caller context; no lwIP
  // calls may originate from interrupts. receive() consumes at most one frame,
  // copies it, and releases DMA ownership; 0 means empty. Oversize frames
  // return their size without copying. transmit() copies before returning,
  // never waits for space. poll() reclaims completed TX; errors() is a
  // cumulative count.
  struct Driver {
    void* context;
    bool (*init)(void*, const Mac&);
    void (*stop)(void*);
    void (*poll)(void*);
    Link (*link)(void*);
    std::size_t (*receive)(void*, std::span<std::uint8_t>);
    bool (*transmit)(void*, std::span<const std::uint8_t>);
    std::uint32_t (*errors)(void*);
  };

  struct Clock {
    void* context;
    std::uint32_t (*milliseconds)(void*);
  };
  struct NetworkState;

  // One active service per process (lwIP NO_SYS global state). No DaveOS
  // dependencies, runtime heap, or worker thread. Driver/clock outlive Service.
  // Initialization is single-use; failure requires a new application run/reset.
  class Service {
   public:
    Service(Driver driver, Clock clock, const Config& config = {});
    ~Service();
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    bool init();
    // Supply hardware-derived configuration at initialization, not
    // construction.
    bool init(const Config& config);
    void poll();
    void stop();

    // Return an independent copy so later polling cannot change a saved sample.
    // cppcheck-suppress returnByReference
    Snapshot snapshot() const { return snapshot_; }

   private:
    friend struct NetworkState;
    void CheckLink();
    Driver driver_;
    Clock clock_;
    Config config_;
    Snapshot snapshot_{};
    std::uint32_t last_link_check_ = 0, stack_errors_ = 0;
    bool attempted_ = false, active_ = false;
  };


}  // namespace daveos::net
