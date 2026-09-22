#include "storage/sd/session.h"

#include <vector>

#include "support.hpp"

namespace {
  namespace hal = daveos::hal;
  namespace spi = hal::spi;
  namespace sd = daveos::storage::sd;
  namespace core = daveos::core;

  // CSD v2: 25 MHz maximum, (0x1dff + 1) * 1024 sectors.
  constexpr std::array<std::uint8_t, 16> kCsd{0x40, 0, 0, 0x32, 0,
                                              0x09, 0, 0, 0x1d, 0xff};
  constexpr std::array<std::uint8_t, 16> kCid{3, 'S', 'D', 'T', 'E', 'S', 'T'};

  // Card model driven through spi::Device: initialization commands, register
  // reads (CSD/CID) and single-sector reads, one transaction at a time.
  struct Card {
    std::span<const spi::Action> pending;
    spi::Callback callback;
    std::vector<std::uint32_t> commands;
    std::vector<std::uint32_t> arguments;
    std::uint32_t corrupt = 0;  // Data command whose CRC is damaged.

    spi::Device device() { return {this, 0, &operations}; }

    static hal::Status Start(void* context, std::size_t,
                             std::span<const spi::Action> actions,
                             spi::Callback callback,
                             std::optional<hal::Duration> timeout) {
      auto& self = *static_cast<Card*>(context);
      REQUIRE(timeout);
      REQUIRE(self.pending.empty());
      self.pending = actions;
      self.callback = callback;
      return hal::Status::ok;
    }

    void Complete() {
      REQUIRE_FALSE(pending.empty());
      if (pending.size() == 2) {
        Initialization();
      } else if (pending.size() == 6) {
        Data();
      } else {
        REQUIRE(pending.size() == 1);
        REQUIRE(pending[0].operation == spi::Operation::idle_clocks);
      }
      const spi::Result result{hal::Status::ok, pending.size(), pending};
      pending = {};
      callback(result);
    }

    void Initialization() {
      const auto cmd = static_cast<std::uint8_t>(pending[0].tx[0] & 0x3f);
      commands.push_back(cmd);
      auto rx = pending[1].rx;
      std::fill(rx.begin(), rx.end(), 0xff);
      rx[0] = cmd == 0 || cmd == 8 || cmd == 55 ? 1 : 0;
      if (cmd == 8) {
        rx[1] = rx[2] = 0;
        rx[3] = 1;
        rx[4] = 0xaa;
      } else if (cmd == 58) {
        rx[1] = 0xc0;
        rx[2] = 0xff;
        rx[3] = 0x80;
        rx[4] = 0;
      }
    }

    // read_actions(): command, R1, token, then payload and CRC16.
    void Data() {
      const auto tx = pending[0].tx;
      const auto cmd = static_cast<std::uint32_t>(tx[0] & 0x3f);
      commands.push_back(cmd);
      arguments.push_back((std::uint32_t{tx[1]} << 24) |
                          (std::uint32_t{tx[2]} << 16) |
                          (std::uint32_t{tx[3]} << 8) | tx[4]);
      pending[1].rx[0] = 0;
      pending[3].rx[0] = 0xfe;
      auto rest = pending[5].rx;
      auto payload = rest.first(rest.size() - 2);
      std::fill(payload.begin(), payload.end(), 0);
      if (cmd == 9) {
        std::copy(kCsd.begin(), kCsd.end(), payload.begin());
      } else if (cmd == 10) {
        std::copy(kCid.begin(), kCid.end(), payload.begin());
      } else {
        payload[0] = static_cast<std::uint8_t>(arguments.back());
      }
      auto crc = sd::crc16(payload);
      if (cmd == corrupt) {
        crc ^= 1;
      }
      rest[rest.size() - 2] = static_cast<std::uint8_t>(crc >> 8);
      rest[rest.size() - 1] = static_cast<std::uint8_t>(crc);
    }

    static inline const spi::Device::Operations operations{Start, nullptr,
                                                           nullptr};
  };

  struct Fixture {
    Card card;
    std::vector<std::uint32_t> speeds;
    std::uint32_t rate = 0;
    int resets = 0;
    hal::Callback<hal::ResetResult> reset_callback;
    sd::Session::Observer observer{};
    std::optional<sd::Session> session;

    void Start() {
      session.emplace(
          card.device(),
          sd::Session::Speed{
              this,
              [](void* p, std::uint32_t hz) {
                auto& f = *static_cast<Fixture*>(p);
                f.speeds.push_back(hz);
                f.rate = hz;
              },
              [](void* p) { return static_cast<Fixture*>(p)->rate; }},
          sd::Session::Reset{this,
                             [](void* p, hal::Callback<hal::ResetResult> done) {
                               auto& f = *static_cast<Fixture*>(p);
                               ++f.resets;
                               f.reset_callback = done;
                               return hal::Status::ok;
                             }},
          sd::Transport::Pump{this, [](void*) { return true; }}, observer);
    }

    // Tick, completing each started transaction, until the session settles.
    void Run() {
      for (int i = 0; i < 10000 && session->running(); ++i) {
        session->tick();
        if (!card.pending.empty()) {
          card.Complete();
        }
      }
      REQUIRE_FALSE(session->running());
    }
  };
}  // namespace

TEST_CASE("SD session initializes, reads the CSD and serves a block lease") {
  Fixture f;
  f.Start();
  auto& session = *f.session;
  auto device = session.block_device();
  CHECK_FALSE(device.ready(device.context));
  CHECK(device.acquire(device.context) == core::Status::not_running);
  REQUIRE(session.request() == hal::Status::ok);
  CHECK(session.request() == hal::Status::busy);
  f.Run();
  CHECK(session.ready());
  CHECK(std::string_view(session.error()) == "ok");
  CHECK(session.ocr() == 0xc0ff8000);
  CHECK(session.card().sectors == 0x1e00 * 1024);
  CHECK(session.card().maximum_hz == 25000000);
  // Startup rate for initialization, then the 1 MHz data rate after the CSD.
  CHECK(f.speeds == std::vector<std::uint32_t>{400000, 1000000});
  CHECK(f.card.commands.back() == 9);
  CHECK(device.sectors(device.context) == session.card().sectors);
  REQUIRE(device.acquire(device.context) == core::Status::ok);
  CHECK(device.acquire(device.context) == core::Status::busy);
  CHECK(session.request() == hal::Status::busy);
  CHECK(session.reset() == hal::Status::busy);
  device.release(device.context);
  CHECK(session.request() == hal::Status::ok);
  CHECK_FALSE(session.ready());
}

TEST_CASE("SD session observer drives diagnostic reads and failures") {
  struct Diagnostics {
    std::vector<std::uint32_t> seen;
    std::vector<bool> completions;
    int starts = 0;
    bool fail_sector = false;
  } d;

  Fixture f;
  f.observer = {
      &d, [](void* p) { ++static_cast<Diagnostics*>(p)->starts; },
      [](void* p, std::uint32_t command, std::span<const std::uint8_t> data) {
        auto& self = *static_cast<Diagnostics*>(p);
        self.seen.push_back(command);
        using Kind = sd::Session::Step::Kind;
        if (command == 9) {
          CHECK(data.size() == 16);
          return sd::Session::Step{Kind::read, 10};
        }
        if (command == 10) {
          CHECK(data[1] == 'S');
          return sd::Session::Step{Kind::read, 17, 7};
        }
        CHECK(data.size() == 512);
        CHECK(data[0] == 7);
        return self.fail_sector
                   ? sd::Session::Step{Kind::fail, 0, 0, "sector mismatch"}
                   : sd::Session::Step{Kind::finish};
      },
      [](void* p, bool success) {
        static_cast<Diagnostics*>(p)->completions.push_back(success);
      }};
  f.Start();
  auto& session = *f.session;
  REQUIRE(session.request() == hal::Status::ok);
  f.Run();
  CHECK(session.ready());
  CHECK(d.starts == 1);
  CHECK(d.seen == std::vector<std::uint32_t>{9, 10, 17});
  CHECK(f.card.arguments == std::vector<std::uint32_t>{0, 0, 7});
  // The observer owns the rate once it takes over: no automatic data rate.
  CHECK(f.speeds == std::vector<std::uint32_t>{400000});
  d.fail_sector = true;
  REQUIRE(session.request() == hal::Status::ok);
  f.Run();
  CHECK_FALSE(session.ready());
  CHECK(std::string_view(session.error()) == "sector mismatch");
  CHECK(session.command() == 17);
  CHECK(d.completions == std::vector<bool>{true, false});
}

TEST_CASE("SD session rejects corrupt data and recovers through reset") {
  Fixture f;
  hal::Status reported = hal::Status::busy;
  f.observer.context = &reported;
  f.observer.reset_completed = [](void* p, hal::Status status) {
    *static_cast<hal::Status*>(p) = status;
  };
  f.card.corrupt = 9;
  f.Start();
  auto& session = *f.session;
  REQUIRE(session.request() == hal::Status::ok);
  f.Run();
  CHECK_FALSE(session.ready());
  CHECK(session.command() == 9);
  CHECK(std::string_view(session.error()) ==
        "missing/error data token, truncated data or CRC mismatch");
  REQUIRE(session.reset() == hal::Status::ok);
  session.tick();
  REQUIRE(f.resets == 1);
  CHECK(session.running());
  f.reset_callback(hal::ResetResult{hal::Status::ok});
  session.tick();
  CHECK_FALSE(session.running());
  CHECK(reported == hal::Status::ok);
  CHECK_FALSE(session.ready());
  f.card.corrupt = 0;
  REQUIRE(session.request() == hal::Status::ok);
  f.Run();
  CHECK(session.ready());
}
