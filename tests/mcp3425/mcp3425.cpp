#include "drivers/mcp3425.h"

#include "hal/adapters/i2c_module.hpp"
#include "hal/controller.hpp"
#include "platform/fake/bus.hpp"
#include "support.hpp"

namespace {
  namespace hal = daveos::hal;
  namespace i2c = hal::i2c;
  namespace fake = daveos::platform::fake;

  struct Fixture {
    fake::BusClock<> clock;
    fake::BusCritical critical;
    fake::I2cBus backend;
    hal::Controller<fake::I2cBus, fake::BusClock<>, fake::BusCritical, 1> bus{
        backend, clock, critical, {{{{0x68}, 100000}}}};
    daveos::drivers::Mcp3425 adc{bus.device<0>()};
    std::array<std::uint8_t, 1> config{0x88};
    std::array<std::uint8_t, 3> receive{};
    std::uint64_t driver_offset = 0;

    Fixture() { REQUIRE(bus.init() == hal::Status::ok); }

    void tick() {
      adc.tick(clock.now() + driver_offset);
      std::size_t count = 0;
      while (backend.take_pending()) {
        REQUIRE(++count < 100);
        bus.interrupt();
      }
      clock.advance(1000);
      while (auto due = clock.take_due()) {
        due();
      }
    }

    void run() {
      for (std::size_t i = 0; i < 300 && !adc.result(); ++i) {
        tick();
      }
      REQUIRE(adc.result());
      REQUIRE_FALSE(adc.busy());
    }
  };
}  // namespace

TEST_CASE("MCP3425 polls readiness and decodes signed 16-bit samples") {
  Fixture f;
  const auto raw = GENERATE(0u, 1u, 0x7fffu, 0x8000u, 0xffffu);
  const std::array<std::uint8_t, 3> busy{0, 0, 0x88};
  const std::array<std::uint8_t, 3> ready{static_cast<std::uint8_t>(raw >> 8),
                                          static_cast<std::uint8_t>(raw), 8};
  const std::array script{fake::I2cBus::Step{i2c::write(f.config)},
                          fake::I2cBus::Step{i2c::read(f.receive), busy},
                          fake::I2cBus::Step{i2c::read(f.receive), ready}};
  for (std::size_t repeat = 0; repeat < 2; ++repeat) {
    f.backend.script(script);
    REQUIRE(f.adc.request() == hal::Status::ok);
    REQUIRE(f.adc.request() == hal::Status::busy);
    REQUIRE_FALSE(f.adc.result());
    f.run();
    const auto code =
        static_cast<std::int32_t>(raw) - (raw >= 32768 ? 65536 : 0);
    REQUIRE(f.adc.result()->status == hal::Status::ok);
    REQUIRE(f.adc.result()->code == code);
    REQUIRE(f.adc.result()->microvolts == code * 125 / 2);
  }
}

TEST_CASE("MCP3425 reports transfer errors and configuration mismatch") {
  Fixture f;
  const auto failure = GENERATE(0, 1, 2, 3);
  const std::array<std::uint8_t, 3> wrong{0, 0, 0x18};
  const std::array script{
      fake::I2cBus::Step{i2c::write(f.config),
                         {},
                         failure == 0 ? hal::Status::nack : hal::Status::ok,
                         failure == 3},
      fake::I2cBus::Step{i2c::read(f.receive), wrong,
                         failure == 1 ? hal::Status::nack : hal::Status::ok}};
  f.backend.script(script);
  REQUIRE(f.adc.request() == hal::Status::ok);
  f.run();
  REQUIRE(f.adc.result()->status == (failure == 3 ? hal::Status::timeout
                                     : failure == 2
                                         ? hal::Status::response_mismatch
                                         : hal::Status::nack));
  REQUIRE(f.adc.result()->code == 0);
}

TEST_CASE(
    "MCP3425 conversion timeout retains in-flight buffers until completion") {
  Fixture f;
  const std::array<std::uint8_t, 3> busy{0, 0, 0x88};
  const std::array script{
      fake::I2cBus::Step{i2c::write(f.config)},
      fake::I2cBus::Step{i2c::read(f.receive), busy, hal::Status::ok, true}};
  f.backend.script(script);
  REQUIRE(f.adc.request() == hal::Status::ok);
  for (std::size_t i = 0; i < 8; ++i) {
    f.tick();
  }
  // Advance the driver's conversion clock independently of the HAL deadline.
  f.driver_offset = 300000;
  f.adc.tick(f.clock.now() + f.driver_offset);
  REQUIRE_FALSE(f.adc.result());
  REQUIRE(f.adc.request() == hal::Status::busy);
  f.run();
  REQUIRE(f.adc.result()->status == hal::Status::timeout);
}

TEST_CASE("MCP3425 times out a converter that never becomes ready") {
  Fixture f;
  const std::array<std::uint8_t, 3> busy{0, 0, 0x88};
  std::array<fake::I2cBus::Step, 64> script{};
  script[0] = {i2c::write(f.config)};
  for (std::size_t i = 1; i < script.size(); ++i) {
    script[i] = {i2c::read(f.receive), busy};
  }
  f.backend.script(script);
  REQUIRE(f.adc.request() == hal::Status::ok);
  f.run();
  REQUIRE(f.adc.result()->status == hal::Status::timeout);
  REQUIRE(f.clock.now() >= 250000);
}

namespace {
  struct NamedBus {
    const char* label;
    bool leased = false, stuck = false;
    std::size_t resets = 0;
    hal::Statistics counters{};
    std::size_t probes = 0, snapshots = 0, releases = 0;
    std::uint8_t responding = 0x68;
    std::optional<std::uint8_t> fail_at{};
    std::vector<std::uint8_t> addresses{};

    const char* name() const { return label; }

    hal::Status init(testing::core::SchedulerInterface<testing::Event>&) {
      return hal::Status::ok;
    }

    void deinit() {}

    bool acquire() {
      if (leased) {
        return false;
      }
      leased = true;
      return true;
    }

    void release() {
      leased = false;
      ++releases;
    }

    hal::Statistics statistics() {
      ++snapshots;
      return counters;
    }

    bool needs_reset() const { return stuck; }

    hal::Status reset(hal::Callback<hal::ResetResult> callback,
                      std::optional<hal::Duration>) {
      REQUIRE(leased);
      ++resets;
      stuck = false;
      callback({hal::Status::ok});
      return hal::Status::ok;
    }

    hal::Status probe(i2c::Address address, i2c::Callback callback,
                      std::optional<hal::Duration>) {
      REQUIRE(leased);
      ++probes;
      addresses.push_back(address.value);
      const auto status = fail_at == address.value      ? hal::Status::timeout
                          : address.value == responding ? hal::Status::ok
                                                        : hal::Status::nack;
      callback({status, status == hal::Status::ok ? 1u : 0u, {}});
      return hal::Status::ok;
    }
  };
}  // namespace

TEST_CASE("I2C diagnostics enumerate and label every configured bus") {
  namespace core = testing::core;
  testing::Fake platform;
  testing::Sink sink;
  NamedBus first{"I2C1"}, second{"I2C2"};
  second.responding = 0x48;
  hal::I2cModule<testing::Event, NamedBus, NamedBus> module{first, second};
  auto logger =
      core::make_logger(platform, core::SubscriberList{sink.subscriber()});
  auto scheduler = core::make_scheduler<testing::Event>(
      platform, core::ModuleList{&module}, logger);
  REQUIRE(scheduler.init() == core::Status::ok);
  // An occupied later bus releases earlier reservations without probing.
  second.leased = true;
  REQUIRE(module.Scan() == core::Status::busy);
  REQUIRE_FALSE(first.leased);
  REQUIRE(first.probes == 0);
  second.leased = false;
  REQUIRE(module.Scan() == core::Status::ok);
  REQUIRE(module.Scan() == core::Status::busy);
  REQUIRE_FALSE(first.acquire());
  for (std::size_t i = 0; i < 230; ++i) {
    module.Tick();
    while (logger.dispatch()) {
    }
  }
  REQUIRE_FALSE(first.leased);
  REQUIRE_FALSE(second.leased);
  for (const auto* bus : {&first, &second}) {
    REQUIRE(bus->addresses.size() == 112);
    REQUIRE(bus->addresses.front() == 8);
    REQUIRE(bus->addresses.back() == 0x77);
  }
  REQUIRE(module.Stats() == core::Status::ok);
  while (logger.dispatch()) {
  }
  REQUIRE(first.snapshots == 1);
  REQUIRE(second.snapshots == 1);
#if DAVEOS_LOGGING
  std::string text;
  for (const auto& record : sink.records) {
    text += record.message + "\n";
  }
  REQUIRE(text.find("I2C1 ACK map") != std::string::npos);
  REQUIRE(text.find("I2C2 ACK map") != std::string::npos);
  REQUIRE(text.find("I2C1: reads=") != std::string::npos);
  REQUIRE(text.find("I2C2: reads=") != std::string::npos);
#endif
}

TEST_CASE("I2C diagnostic errors do not hide later configured buses") {
  namespace core = testing::core;
  testing::Fake platform;
  NamedBus first{"I2C1"}, second{"I2C2"};
  first.fail_at = 9;
  hal::I2cModule<testing::Event, NamedBus, NamedBus> module{first, second};
  auto scheduler =
      core::make_scheduler<testing::Event>(platform, core::ModuleList{&module});
  REQUIRE(scheduler.init() == core::Status::ok);
  REQUIRE(module.Scan() == core::Status::ok);
  for (std::size_t i = 0; i < 120; ++i) {
    module.Tick();
  }
  REQUIRE(first.probes == 2);
  REQUIRE(second.probes == 112);
  REQUIRE_FALSE(first.leased);
  REQUIRE_FALSE(second.leased);
  REQUIRE(module.Scan() == core::Status::ok);
}

TEST_CASE("I2C module defers startup recovery and marks counter overflow") {
  namespace core = testing::core;
  testing::Fake platform;
  testing::Sink sink;
  NamedBus bus{"I2C1"};
  bus.stuck = true;
  bus.counters.read_attempts = std::uint64_t{UINT32_MAX} + 1;
  hal::I2cModule<testing::Event, NamedBus> module{bus};
  auto logger =
      core::make_logger(platform, core::SubscriberList{sink.subscriber()});
  auto scheduler = core::make_scheduler<testing::Event>(
      platform, core::ModuleList{&module}, logger);
  REQUIRE(scheduler.init() == core::Status::ok);
  REQUIRE(bus.resets == 0);
  REQUIRE(bus.leased);
  REQUIRE(module.Scan() == core::Status::busy);
  module.Tick();
  module.Tick();
  REQUIRE(bus.resets == 1);
  REQUIRE_FALSE(bus.leased);
  REQUIRE(module.Stats() == core::Status::ok);
  while (logger.dispatch()) {
  }
#if DAVEOS_LOGGING
  REQUIRE(std::any_of(
      sink.records.begin(), sink.records.end(), [](const auto& record) {
        return record.message.find("reads=4294967295+") != std::string::npos;
      }));
#endif
}
