#include "hal/controller.hpp"
#include "hal/i2c/bus_clear.h"
#include "platform/fake/bus.hpp"
#include "support.hpp"

namespace {
  namespace hal = daveos::hal;
  namespace i2c = hal::i2c;
  namespace fake = daveos::platform::fake;
  namespace chrono = std::chrono;

  struct Pins {
    std::uint64_t now = 0, stretch_until = 0;
    std::size_t pulses = 0, release_after = 3, enters = 0, leaves = 0;
    bool scl = true, sda = true, observed_scl = true, gpio = false,
         stop = false;
    std::vector<std::pair<std::uint64_t, bool>> scl_edges;

    bool high() {
      const bool level = scl && now >= stretch_until;
      if (level && !observed_scl && sda) {
        ++pulses;
      }
      observed_scl = level;
      return level;
    }

    i2c::BusClearPins operations() {
      return {this,
              [](void* p) {
                auto& s = *static_cast<Pins*>(p);
                s.gpio = true;
                ++s.enters;
              },
              [](void* p) {
                auto& s = *static_cast<Pins*>(p);
                s.scl = s.sda = true;
                s.gpio = false;
                ++s.leaves;
              },
              [](void* p, bool release) {
                auto& s = *static_cast<Pins*>(p);
                s.scl = release;
                if (!release) {
                  s.observed_scl = false;
                }
                s.scl_edges.emplace_back(s.now, release);
              },
              [](void* p, bool release) {
                auto& s = *static_cast<Pins*>(p);
                if (release && !s.sda) {
                  REQUIRE(s.high());
                  s.stop = true;
                }
                s.sda = release;
              },
              [](void* p) { return static_cast<Pins*>(p)->high(); },
              [](void* p) {
                const auto& s = *static_cast<Pins*>(p);
                return s.sda && s.pulses >= s.release_after;
              }};
    }
  };

  struct Backend : fake::I2cBus {
    Pins pins;
    i2c::BusClear clear{pins.operations()};
    bool stuck = true, aborted = false;

    bool needs_reset() const { return stuck; }

    void start_reset() { clear.start(); }

    std::optional<hal::Status> poll_reset(std::uint64_t now) {
      pins.now = now;
      const auto result = clear.poll(now);
      if (result) {
        stuck = *result != hal::Status::ok;
      }
      return result;
    }

    void abort_reset() {
      clear.abort();
      aborted = true;
    }
  };

  struct Fixture {
    fake::BusClock<> clock;
    fake::BusCritical critical;
    Backend backend;
    hal::Controller<Backend, fake::BusClock<>, fake::BusCritical, 1> bus{
        backend, clock, critical, {{{{0x68}, 100000}}}};
    hal::Status result = hal::Status::busy;
    std::size_t completions = 0;

    Fixture() { REQUIRE(bus.init() == hal::Status::ok); }

    void Done(const hal::ResetResult& r) {
      result = r.status;
      ++completions;
    }

    void pump() {
      std::size_t count = 0;
      while (backend.take_pending()) {
        REQUIRE(++count < 100);
        bus.interrupt();
      }
    }

    void advance(std::uint64_t us) {
      clock.advance(us);
      while (auto alarm = clock.take_due()) {
        alarm();
      }
      pump();
    }

    void start() {
      REQUIRE(bus.reset(
                  hal::Callback<hal::ResetResult>::bind<&Fixture::Done>(*this),
                  chrono::milliseconds{1}) == hal::Status::ok);
      pump();
    }

    void finish() {
      for (std::size_t i = 0; i < 120 && !completions; ++i) {
        advance(10);
      }
      REQUIRE(completions == 1);
      REQUIRE_FALSE(backend.pins.gpio);
      REQUIRE(backend.pins.scl);
      REQUIRE(backend.pins.sda);
      REQUIRE(clock.pending() == 0);
    }
  };
}  // namespace

TEST_CASE("I2C bus clear sends at most nine clocks and a STOP") {
  Fixture f;
  const auto release = GENERATE(0u, 1u, 3u, 9u, 10u);
  f.backend.pins.release_after = release;
  REQUIRE(f.bus.faulted());
  f.start();
  f.finish();
  REQUIRE(f.result == (release > 9 ? hal::Status::faulted : hal::Status::ok));
  REQUIRE(f.bus.faulted() == (release > 9));
  REQUIRE(f.backend.pins.pulses == std::min(release, 9u));
  REQUIRE(f.backend.pins.stop == (release != 0));
  const auto& edges = f.backend.pins.scl_edges;
  for (std::size_t i = 1; i < edges.size(); ++i) {
    REQUIRE(edges[i].first - edges[i - 1].first >= 5);
  }
  REQUIRE(f.backend.pins.leaves == 1);
}

TEST_CASE(
    "I2C recovery waits for stretched SCL and times out a held-low clock") {
  Fixture f;
  const bool permanent = GENERATE(false, true);
  f.backend.pins.stretch_until = permanent ? 1000000 : 100;
  f.start();
  f.advance(90);
  REQUIRE(f.backend.pins.pulses == 0);
  REQUIRE(f.completions == 0);
  f.finish();
  REQUIRE(f.result == (permanent ? hal::Status::timeout : hal::Status::ok));
  REQUIRE(f.backend.aborted == permanent);
  REQUIRE(f.bus.faulted() == permanent);
}

TEST_CASE("I2C reset timer failure releases GPIO and allows another reset") {
  Fixture f;
  f.start();
  f.clock.fail_arm = true;
  f.advance(10);
  f.finish();
  REQUIRE(f.result == hal::Status::timer_error);
  REQUIRE(f.bus.faulted());
  REQUIRE(f.backend.aborted);
  f.clock.fail_arm = false;
  f.completions = 0;
  f.start();
  f.finish();
  REQUIRE(f.result == hal::Status::ok);
  REQUIRE_FALSE(f.bus.faulted());
  REQUIRE(f.bus.statistics().resets == 2);
  REQUIRE(f.bus.statistics().reset_failures == 1);
}

TEST_CASE("I2C recovery abort releases pins at every waveform stage") {
  const auto ticks = GENERATE(0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u);
  Pins pins;
  pins.release_after = 1;
  i2c::BusClear clear{pins.operations()};
  clear.start();
  for (std::size_t i = 0; i < ticks; ++i) {
    pins.now += 5;
    (void)clear.poll(pins.now);
  }
  clear.abort();
  REQUIRE_FALSE(pins.gpio);
  REQUIRE(pins.scl);
  REQUIRE(pins.sda);
  pins.release_after = 0;
  clear.start();
  std::optional<hal::Status> result;
  for (std::size_t i = 0; i < 20 && !result; ++i) {
    pins.now += 5;
    result = clear.poll(pins.now);
  }
  REQUIRE(result == hal::Status::ok);
}

TEST_CASE("Controller probes preserve device addresses and device counters") {
  Fixture f;
  f.start();
  f.finish();
  const std::array<std::uint8_t, 1> bytes{0x88};
  const std::array actions{i2c::write(bytes)};
  const std::array script{
      fake::I2cBus::Step{i2c::probe(), {}, hal::Status::nack},
      fake::I2cBus::Step{actions[0]}};
  f.backend.script(script);
  i2c::Result probe_result;
  const i2c::Callback callback{
      &probe_result,
      [](void* p, const i2c::Result& r) { *static_cast<i2c::Result*>(p) = r; }};
  for (auto address : {0u, 7u, 0x78u, 0xffu}) {
    REQUIRE(f.bus.probe({static_cast<std::uint8_t>(address)}, callback) ==
            hal::Status::invalid_argument);
  }
  REQUIRE(f.bus.probe({0x40}, {}) == hal::Status::invalid_argument);
  REQUIRE(f.bus.probe({0x40}, callback) == hal::Status::ok);
  REQUIRE(f.bus.probe({0x41}, callback) == hal::Status::busy);
  i2c::Completion write;
  REQUIRE(write.start(f.bus.device<0>(), actions) == hal::Status::busy);
  f.pump();
  REQUIRE(probe_result.status == hal::Status::nack);
  REQUIRE(probe_result.actions.size() == 1);
  REQUIRE(probe_result.actions[0].operation == i2c::Operation::probe);
  REQUIRE(f.bus.device<0>().statistics().accepted == 0);
  REQUIRE(write.start(f.bus.device<0>(), actions) == hal::Status::ok);
  f.pump();
  REQUIRE(write.result()->status == hal::Status::ok);
  REQUIRE(f.backend.trace[0].address == 0x40);
  REQUIRE(f.backend.trace[1].address == 0x68);
  REQUIRE(f.bus.device<0>().statistics().accepted == 1);
  REQUIRE(f.bus.statistics().accepted == 2);
}
