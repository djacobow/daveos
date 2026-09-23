#include "drivers/ssd1306.h"

#include "drivers/adapters/ssd1306.hpp"
#include "drivers/debug_display.hpp"
#include "support.hpp"

namespace {
  namespace hal = daveos::hal;
  namespace i2c = hal::i2c;

  struct Bus {
    std::vector<std::vector<std::uint8_t>> packets;
    std::span<const i2c::Action> pending;
    i2c::Callback callback;
    bool leased = false;

    bool acquire() {
      if (leased) {
        return false;
      }
      leased = true;
      return true;
    }

    void release() { leased = false; }

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

namespace {
  struct Screen {
    daveos::drivers::DisplayLines frame{};
    bool enabled = true;
    std::size_t frames = 0;

    bool live() const { return enabled; }

    daveos::core::Status show(const daveos::drivers::DisplayLines& value) {
      frame = value;
      ++frames;
      return daveos::core::Status::ok;
    }
  };

  struct Clock {
    std::uint64_t time = 0;

    std::uint64_t now() const { return time; }
  };
}  // namespace

TEST_CASE("debug display renders bounded eight-row status at one Hz") {
  Clock clock;
  Screen screen;
  using Module = daveos::drivers::DebugDisplay<Clock, Screen>;
  constexpr auto tasks = Module::tasks();
  STATIC_REQUIRE(tasks.size() == 1);
  STATIC_REQUIRE(tasks[0].period == 1000000);
  daveos::util::Version version{1, 2, 345};
  std::vector<std::size_t> calls;
  Module module(
      clock, screen, version,
      {&calls, [](void* context, std::size_t row, std::span<char> out) {
         static_cast<std::vector<std::size_t>*>(context)->push_back(row);
         if (row == 0) {
           std::fill(out.begin(), out.end(), 'X');
         }
         if (row == 1) {
           out[0] = '\n';
           out[1] = static_cast<char>(0xff);
         }
         if (row == 5) {
           std::snprintf(out.data(), out.size(), "ABC 123");
         }
       }});
  CHECK(calls.empty());
  clock.time = (std::uint64_t{2} * 86400 + 3 * 3600 + 4 * 60 + 5) * 1000000;
  module.Update();
  CHECK(screen.frames == 1);
  CHECK(std::string_view(screen.frame[0].data()) == "Up:2d 03:04:05");
  CHECK(std::string_view(screen.frame[1].data()) == std::string(21, 'X'));
  CHECK(std::string_view(screen.frame[2].data()) == "??");
  CHECK(std::string_view(screen.frame[6].data()) == "SN:ABC 123");
  CHECK(std::string_view(screen.frame[7].data()) == "FW:1.2.345");
  CHECK(calls == std::vector<std::size_t>{0, 1, 2, 3, 4, 5});
  screen.enabled = false;
  module.Update();
  CHECK(screen.frames == 1);
}

TEST_CASE("debug display has honest defaults and handles long uptime") {
  Clock clock{std::numeric_limits<std::uint64_t>::max()};
  Screen screen;
  daveos::drivers::DebugDisplay module(clock, screen, daveos::util::Version{});
  module.Update();
  CHECK(std::string_view(screen.frame[6].data()) == "SN:(none)");
  CHECK(std::string_view(screen.frame[7].data()) == "FW:0.0 local");
  for (const auto& line : screen.frame) {
    CHECK(line.back() == 0);
  }
}

TEST_CASE(
    "automatic display failure silently disables retries and releases bus") {
  Bus bus;
  daveos::drivers::Ssd1306Module screen(bus, bus.device);
  Clock clock;
  daveos::drivers::DebugDisplay renderer(clock, screen,
                                         daveos::util::Version{});
  const bool initialized = GENERATE(false, true);
  if (initialized) {
    renderer.Update();
    for (int i = 0; i < 40; ++i) {
      screen.Tick();
      if (!bus.pending.empty()) {
        bus.Complete();
      }
    }
  }
  const auto previous = bus.packets.size();
  renderer.Update();
  CHECK(bus.leased);
  screen.Tick();
  bus.Complete(hal::Status::nack);
  screen.Tick();
  CHECK_FALSE(bus.leased);
  CHECK_FALSE(screen.live());
  for (int i = 0; i < 5; ++i) {
    renderer.Update();
    screen.Tick();
  }
  CHECK(bus.packets.size() == previous + 1);
  CHECK(bus.pending.empty());
  // No scheduler/logger is bound: this also checks the silent path never logs.
}

TEST_CASE(
    "automatic display skips a leased bus then initializes without blocking") {
  Bus bus;
  daveos::drivers::Ssd1306Module screen(bus, bus.device);
  daveos::drivers::DisplayLines lines{};
  bus.leased = true;
  CHECK(screen.show(lines) == daveos::core::Status::busy);
  CHECK(screen.live());
  bus.leased = false;
  REQUIRE(screen.show(lines) == daveos::core::Status::ok);
  for (int i = 0; i < 40; ++i) {
    screen.Tick();
    if (!bus.pending.empty()) {
      bus.Complete();
    }
  }
  CHECK(bus.packets.size() == 18);
  CHECK_FALSE(bus.leased);
  REQUIRE(screen.show(lines) == daveos::core::Status::ok);
  for (int i = 0; i < 40; ++i) {
    screen.Tick();
    if (!bus.pending.empty()) {
      bus.Complete();
    }
  }
  CHECK(bus.packets.size() == 34);
  CHECK_FALSE(bus.leased);
}
