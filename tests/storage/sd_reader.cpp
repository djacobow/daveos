#include "hal/controller.hpp"
#include "platform/fake/bus.hpp"
#include "storage/sd/transport.h"
#include "support.hpp"

namespace {
  namespace hal = daveos::hal;
  namespace fake = daveos::platform::fake;
  namespace sd = daveos::storage::sd;

  struct Fixture {
    fake::BusClock<> clock;
    fake::BusCritical critical;
    fake::SpiBus backend;
    hal::Controller<fake::SpiBus, fake::BusClock<>, fake::BusCritical, 1> bus{
        backend, clock, critical, {{{1, 1000000}}}};
    std::array<std::uint8_t, sd::Transport::kCaptureBytes> scratch{};
    bool keep_going = true;
    std::uint32_t pumps = 0;
    sd::Transport reader{
        bus.device<0>(),
        scratch,
        1000000,
        8192,
        {this, [](void* p) { return static_cast<Fixture*>(p)->Pump(); }}};

    Fixture() { REQUIRE(bus.init() == hal::Status::ok); }

    bool Pump() {
      ++pumps;
      while (backend.take_pending()) {
        bus.interrupt();
      }
      clock.advance(1000);
      while (auto callback = clock.take_due()) {
        callback();
      }
      return keep_going;
    }
  };
}  // namespace

TEST_CASE("SD command CRC and asynchronous sector adapter validate data") {
  REQUIRE(sd::command(0, 0)[5] == 0x95);
  REQUIRE(sd::command(8, 0x1aa)[5] == 0x87);
  Fixture f;
  std::array<std::uint8_t, sd::Transport::kCaptureBytes - 1> wire;
  wire.fill(0xff);
  wire[0] = 0;
  wire[1] = 0xfe;
  wire[514] = 0x7f;
  wire[515] = 0xa1;  // 512 bytes of ff, CRC16.
  const auto tx = sd::command(17, 5);
  const std::array script{fake::SpiBus::Step{hal::spi::write(tx)},
                          fake::SpiBus::Step{hal::spi::read(f.scratch), wire},
                          fake::SpiBus::Step{hal::spi::idle_clocks(8)}};
  f.backend.script(script);
  std::array<std::uint8_t, 512> bytes{};
  REQUIRE(f.reader.read(5, bytes));
  CHECK(std::all_of(bytes.begin(), bytes.end(),
                    [](auto byte) { return byte == 0xff; }));
  CHECK(f.pumps > 1);
  CHECK_FALSE(f.reader.read(8192, bytes));
  CHECK_FALSE(f.reader.read(0, std::span{bytes}.first(1)));
  wire[514] ^= 1;
  f.backend.script(script);
  CHECK_FALSE(f.reader.read(5, bytes));
}

TEST_CASE("SD reader rejects forbidden waits before touching hardware") {
  Fixture f;
  f.keep_going = false;
  std::array<std::uint8_t, 512> bytes{};
  CHECK_FALSE(f.reader.read(0, bytes));
  CHECK(f.backend.begins == 0);
}

TEST_CASE("SD reader waits for timeout before releasing buffers after abort") {
  Fixture f;
  const auto tx = sd::command(17, 0);
  const std::array script{
      fake::SpiBus::Step{hal::spi::write(tx), {}, hal::Status::ok, true}};
  f.backend.script(script);
  // Preflight succeeds; the first in-flight pump requests abort. Hardware is
  // hung, so the reader must continue pumping until the HAL timeout fires.
  sd::Transport reader{f.bus.device<0>(),
                       f.scratch,
                       1000000,
                       8192,
                       {&f, [](void* p) {
                          auto& fixture = *static_cast<Fixture*>(p);
                          const auto first = fixture.pumps == 0;
                          fixture.Pump();
                          return first;
                        }}};
  std::array<std::uint8_t, 512> bytes{};
  CHECK_FALSE(reader.read(0, bytes));
  CHECK(f.pumps >= 250);
  // A new transaction succeeds in acquiring the bus after timeout cleanup.
  f.backend.script({});
  hal::spi::Completion completion;
  const std::array gap{hal::spi::idle_clocks(8)};
  CHECK(completion.start(f.bus.device<0>(), gap,
                         std::chrono::milliseconds{1}) == hal::Status::ok);
  while (!completion.ready()) {
    f.Pump();
  }
}

TEST_CASE("SD reader handles consecutive sectors and rejects reentry") {
  Fixture f;
  std::array<std::uint8_t, sd::Transport::kCaptureBytes - 1> wire;
  wire.fill(0xff);
  wire[0] = 0;
  wire[1] = 0xfe;
  wire[514] = 0x7f;
  wire[515] = 0xa1;
  const auto first = sd::command(17, 10), second = sd::command(17, 11);
  const std::array script{fake::SpiBus::Step{hal::spi::write(first)},
                          fake::SpiBus::Step{hal::spi::read(f.scratch), wire},
                          fake::SpiBus::Step{hal::spi::idle_clocks(8)},
                          fake::SpiBus::Step{hal::spi::write(second)},
                          fake::SpiBus::Step{hal::spi::read(f.scratch), wire},
                          fake::SpiBus::Step{hal::spi::idle_clocks(8)}};
  f.backend.script(script);

  struct Context {
    Fixture& fixture;
    sd::Transport* reader = nullptr;
  } context{f};

  sd::Transport reader{f.bus.device<0>(),
                       f.scratch,
                       1000000,
                       8192,
                       {&context, [](void* p) {
                          auto& c = *static_cast<Context*>(p);
                          std::array<std::uint8_t, 512> nested{};
                          CHECK_FALSE(c.reader->read(0, nested));
                          return c.fixture.Pump();
                        }}};
  context.reader = &reader;
  std::array<std::uint8_t, 1024> bytes{};
  REQUIRE(reader.read(10, bytes));
  CHECK(std::all_of(bytes.begin(), bytes.end(),
                    [](auto byte) { return byte == 0xff; }));
  CHECK(
      f.backend.begins ==
      4);  // Two command/data transactions and two trailing-clock transactions.
}

TEST_CASE(
    "SD writes gate payload and require acceptance ready and clean status") {
  for (int failure = 0; failure < 5; ++failure) {
    Fixture f;
    const auto command = sd::command(24, 7);
    const auto status_command = sd::command(13, 0);
    std::array<std::uint8_t, 512> payload{};
    payload.fill(0x35);
    const auto crc = sd::crc16(payload);
    const std::array<std::uint8_t, 2> prefix{0xff, 0xfe};
    const std::array<std::uint8_t, 2> suffix{
        static_cast<std::uint8_t>(crc >> 8), static_cast<std::uint8_t>(crc)};
    std::array<std::uint8_t, 8> r1{}, response{};
    r1.fill(0xff);
    response.fill(0xff);
    r1[1] = failure == 1 ? 4 : 0;
    response[0] = failure == 2 ? 0x0b : 0xe5;
    // Busy release can straddle a byte: only the final ready sample must be ff.
    std::array<std::uint8_t, 8> busy{}, ready{};
    ready.fill(failure == 3 ? 0 : 0xff);
    if (failure != 3) {
      ready[0] = 3;
    }
    std::array<std::uint8_t, 10> status{};
    status.fill(0xff);
    status[0] = 0;
    status[1] = failure == 4 ? 0x20 : 0;
    const std::array script{
        fake::SpiBus::Step{hal::spi::write(command)},
        fake::SpiBus::Step{hal::spi::read(f.scratch), r1},
        fake::SpiBus::Step{hal::spi::write(prefix)},
        fake::SpiBus::Step{hal::spi::write(payload)},
        fake::SpiBus::Step{hal::spi::write(suffix)},
        fake::SpiBus::Step{hal::spi::read(f.scratch), response},
        fake::SpiBus::Step{hal::spi::read(f.scratch), busy},
        fake::SpiBus::Step{hal::spi::read(f.scratch), ready},
        failure == 3 ? fake::SpiBus::Step{hal::spi::read(f.scratch), busy,
                                          hal::Status::ok, true}
                     : fake::SpiBus::Step{hal::spi::idle_clocks(8)},
        fake::SpiBus::Step{hal::spi::write(status_command)},
        fake::SpiBus::Step{hal::spi::read(f.scratch), status},
        fake::SpiBus::Step{hal::spi::idle_clocks(8)}};
    f.backend.script(script);
    CHECK(f.reader.write(7, payload) == (failure == 0));
    CHECK_FALSE(f.backend.selected);
    if (failure == 1) {
      CHECK(f.backend.trace_count == 2);
    }
    if (failure == 2) {
      CHECK(f.backend.trace_count == 6);
    }
    if (failure == 3) {
      CHECK(f.pumps >= 1000);
      CHECK(f.reader.write_diagnostics().status == hal::Status::timeout);
    } else {
      CHECK(f.pumps < 500);
    }
  }
}
