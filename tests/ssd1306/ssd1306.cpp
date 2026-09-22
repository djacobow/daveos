#include "drivers/ssd1306.h"

#include "support.hpp"

namespace {
  namespace hal = daveos::hal;
  namespace i2c = hal::i2c;

  struct Bus {
    std::vector<std::vector<std::uint8_t>> packets;
    std::span<const i2c::Action> pending;
    i2c::Callback callback;
    hal::Status admission = hal::Status::ok;

    static const i2c::Device::Operations& Operations() {
      static constexpr i2c::Device::Operations operations{
          [](void* p, std::size_t, std::span<const i2c::Action> actions,
             i2c::Callback callback, std::optional<hal::Duration> timeout) {
            auto& bus = *static_cast<Bus*>(p);
            REQUIRE(bus.pending.empty());
            REQUIRE(actions.size() == 1);
            REQUIRE(actions[0].operation == i2c::Operation::write);
            REQUIRE(timeout->microseconds() == 100000);
            if (bus.admission != hal::Status::ok) {
              return bus.admission;
            }
            bus.pending = actions;
            bus.callback = callback;
            return hal::Status::ok;
          },
          [](void*, std::size_t) { return hal::Statistics{}; },
          [](void*, std::size_t) {}};
      return operations;
    }

    i2c::Device device{this, 0, &Operations()};
    daveos::drivers::Ssd1306 display{device};

    void Complete(hal::Status status = hal::Status::ok) {
      REQUIRE_FALSE(pending.empty());
      packets.emplace_back(pending[0].tx.begin(), pending[0].tx.end());
      const auto actions = pending;
      pending = {};
      callback({status, status == hal::Status::ok ? 1u : 0u, actions});
    }

    void Run() {
      for (int i = 0; i < 100 && display.busy(); ++i) {
        display.tick();
        if (!pending.empty()) {
          Complete();
        }
      }
      REQUIRE_FALSE(display.busy());
      REQUIRE(display.result() == hal::Status::ok);
    }
  };
}  // namespace

TEST_CASE("SSD1306 initializes off, clears every page then turns on") {
  Bus b;
  CHECK(b.display.refresh() == hal::Status::not_initialized);
  REQUIRE(b.display.initialize() == hal::Status::ok);
  b.Run();
  REQUIRE(b.packets.size() == 18);
  CHECK(b.packets.front()[0] == 0);
  CHECK(b.packets.front()[1] == 0xae);
  CHECK(b.packets.back() == std::vector<std::uint8_t>{0, 0xaf});
  for (std::size_t page = 0; page < 8; ++page) {
    CHECK(b.packets[1 + page * 2] ==
          std::vector<std::uint8_t>{0, static_cast<std::uint8_t>(0xb0 + page),
                                    0, 0x10});
    auto expected = std::vector<std::uint8_t>(129, 0);
    expected[0] = 0x40;
    CHECK(b.packets[2 + page * 2] == expected);
  }
  CHECK(b.display.initialized());
  b.packets.clear();
  CHECK(b.display.pixel(127, 63) == hal::Status::ok);
  CHECK(b.display.pixel(128, 0) == hal::Status::invalid_argument);
  CHECK(b.display.text(0, 0, "A") == hal::Status::ok);
  CHECK(b.display.text(123, 0, "A") == hal::Status::invalid_argument);
  CHECK(b.display.text(0, 57, "A") == hal::Status::invalid_argument);
  CHECK(b.display.text(0, 0, "\n") == hal::Status::invalid_argument);
  REQUIRE(b.display.refresh() == hal::Status::ok);
  b.Run();
  REQUIRE(b.packets.size() == 16);
  CHECK(b.packets.back()[128] == 0x80);
  CHECK(std::any_of(b.packets[1].begin() + 1, b.packets[1].end(),
                    [](auto c) { return c != 0; }));
}

TEST_CASE(
    "SSD1306 holds buffers until completion and reports every failing stage") {
  const auto fail_at = GENERATE(0, 1, 2, 16, 17);
  Bus b;
  REQUIRE(b.display.initialize() == hal::Status::ok);
  for (int i = 0; i <= fail_at; ++i) {
    b.display.tick();
    REQUIRE_FALSE(b.pending.empty());
    const auto bytes = std::vector<std::uint8_t>(b.pending[0].tx.begin(),
                                                 b.pending[0].tx.end());
    for (int tick = 0; tick < 3; ++tick) {
      b.display.tick();
    }
    CHECK(b.display.initialize() == hal::Status::busy);
    CHECK(b.display.refresh() == hal::Status::busy);
    CHECK(b.display.fill(true) == hal::Status::busy);
    CHECK(b.display.pixel(0, 0) == hal::Status::busy);
    CHECK(b.display.text(0, 0, "x") == hal::Status::busy);
    CHECK_FALSE(b.display.result());
    CHECK(std::equal(bytes.begin(), bytes.end(), b.pending[0].tx.begin()));
    b.Complete(i == fail_at ? hal::Status::timeout : hal::Status::ok);
  }
  b.display.tick();
  REQUIRE(b.display.result() == hal::Status::timeout);
  CHECK_FALSE(b.display.initialized());
  CHECK_FALSE(b.display.busy());
  CHECK(b.display.refresh() == hal::Status::not_initialized);
  REQUIRE(b.display.initialize() == hal::Status::ok);
  b.Run();
}

TEST_CASE("SSD1306 reports rejected submission without waiting for callback") {
  Bus b;
  b.admission = hal::Status::faulted;
  REQUIRE(b.display.initialize() == hal::Status::ok);
  b.display.tick();
  b.display.tick();
  CHECK(b.display.result() == hal::Status::faulted);
  CHECK_FALSE(b.display.busy());
}
