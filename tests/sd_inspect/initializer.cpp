#include "storage/sd/initializer.h"

#include "support.hpp"

namespace {
  namespace hal = daveos::hal;
  namespace spi = hal::spi;
  namespace sd = daveos::storage::sd;

  struct Card {
    std::span<const spi::Action> pending;
    spi::Callback callback;
    std::vector<std::uint32_t> commands;
    std::vector<std::uint64_t> clocks;
    hal::Status rejection = hal::Status::ok;
    std::uint32_t idle = 0;
    std::uint32_t corrupt = 255;
    std::size_t response_offset = 0;
    bool high_capacity = true;
    bool bad_echo = false;
    bool powered_up = true;

    spi::Device device() { return {this, 0, &operations}; }

    static hal::Status Start(void* context, std::size_t,
                             std::span<const spi::Action> actions,
                             spi::Callback callback,
                             std::optional<hal::Duration> timeout) {
      auto& self = *static_cast<Card*>(context);
      REQUIRE(timeout);
      REQUIRE(timeout->microseconds() == 250000);
      REQUIRE(self.pending.empty());
      if (self.rejection != hal::Status::ok) {
        return self.rejection;
      }
      self.pending = actions;
      self.callback = callback;
      return hal::Status::ok;
    }

    void Complete(hal::Status status = hal::Status::ok) {
      REQUIRE_FALSE(pending.empty());
      if (pending.size() == 1) {
        REQUIRE(pending[0].operation == spi::Operation::idle_clocks);
        clocks.push_back(pending[0].amount);
      } else {
        REQUIRE(pending.size() == 2);
        REQUIRE(pending[0].operation == spi::Operation::write);
        REQUIRE(pending[1].operation == spi::Operation::read);
        const auto tx = pending[0].tx;
        const auto cmd = static_cast<std::uint8_t>(tx[0] & 0x3f);
        commands.push_back(cmd);
        const auto expected = sd::command(cmd, cmd == 8    ? 0x1aa
                                               : cmd == 41 ? 0x40000000
                                                           : 0);
        REQUIRE(
            std::equal(tx.begin(), tx.end(), expected.begin(), expected.end()));
        auto rx = pending[1].rx;
        std::fill(rx.begin(), rx.end(), 0xff);
        auto r = response_offset;
        rx[r] = cmd == 58 || (cmd == 41 && idle == 0) ? 0 : 1;
        if (cmd == 41 && idle) {
          --idle;
        }
        if (cmd == 8) {
          rx[r + 1] = rx[r + 2] = 0;
          rx[r + 3] = 1;
          rx[r + 4] = bad_echo ? 0xab : 0xaa;
        }
        if (cmd == 58) {
          rx[r + 1] = (high_capacity ? 0x40 : 0) | (powered_up ? 0x80 : 0);
          rx[r + 2] = 0xff;
          rx[r + 3] = 0x80;
          rx[r + 4] = 0;
        }
        if (cmd == corrupt) {
          rx[r] = 4;
        }
      }
      const spi::Result result{
          status, status == hal::Status::ok ? pending.size() : 0, pending};
      pending = {};
      callback(result);
    }

    void Run(sd::Initializer& init) {
      for (std::size_t i = 0; i < 10000 && !init.result(); ++i) {
        init.tick();
        if (!pending.empty()) {
          Complete();
        }
      }
      REQUIRE(init.result());
      REQUIRE_FALSE(init.busy());
      REQUIRE(pending.empty());
    }

    static inline const spi::Device::Operations operations{Start, nullptr,
                                                           nullptr};
  };
}  // namespace

TEST_CASE("SD initialization preserves clocks, command framing and restart") {
  Card card;
  sd::Initializer init{card.device()};
  REQUIRE_FALSE(init.result());
  init.tick();
  REQUIRE(card.pending.empty());
  for (std::size_t offset : {0u, 7u}) {
    card.response_offset = offset;
    card.idle = 2;
    card.commands.clear();
    card.clocks.clear();
    REQUIRE(init.request() == hal::Status::ok);
    REQUIRE(init.request() == hal::Status::busy);
    init.tick();
    REQUIRE_FALSE(card.pending.empty());
    for (int i = 0; i < 10; ++i) {
      init.tick();
    }
    REQUIRE_FALSE(init.result());
    card.Complete();
    card.Run(init);
    REQUIRE(init.result()->status == hal::Status::ok);
    REQUIRE(init.result()->ocr == 0xc0ff8000);
    REQUIRE(init.result()->command == 58);
    REQUIRE(card.commands ==
            std::vector<std::uint32_t>{0, 8, 55, 41, 55, 41, 55, 41, 58});
    REQUIRE(card.clocks.front() == 80);
    REQUIRE(card.clocks.size() == card.commands.size());
    REQUIRE(std::all_of(card.clocks.begin() + 1, card.clocks.end(),
                        [](auto n) { return n == 8; }));
  }
}

TEST_CASE("SD initialization rejects malformed or unsupported cards") {
  for (const auto command : {0u, 8u, 55u, 41u, 58u}) {
    Card card;
    card.corrupt = command;
    sd::Initializer init{card.device()};
    REQUIRE(init.request() == hal::Status::ok);
    card.Run(init);
    REQUIRE(init.result()->status == hal::Status::response_mismatch);
    REQUIRE(init.result()->command == command);
  }
  Card card;
  sd::Initializer init{card.device()};
  card.high_capacity = false;
  REQUIRE(init.request() == hal::Status::ok);
  card.Run(init);
  REQUIRE(init.result()->status == hal::Status::response_mismatch);
  card.high_capacity = true;
  card.response_offset = 8;
  REQUIRE(init.request() == hal::Status::ok);
  card.Run(init);
  REQUIRE(init.result()->status == hal::Status::response_mismatch);
}

TEST_CASE("SD initialization bounds idle retries and reports HAL failures") {
  Card card;
  sd::Initializer init{card.device()};
  card.idle = 1000;
  REQUIRE(init.request() == hal::Status::ok);
  card.Run(init);
  REQUIRE(init.result()->status == hal::Status::response_mismatch);
  REQUIRE(std::count(card.commands.begin(), card.commands.end(), 41) == 1000);
  card.rejection = hal::Status::busy;
  REQUIRE(init.request() == hal::Status::ok);
  card.Run(init);
  REQUIRE(init.result()->status == hal::Status::busy);
  card.rejection = hal::Status::ok;
  REQUIRE(init.request() == hal::Status::ok);
  init.tick();
  card.Complete(hal::Status::timeout);
  card.Run(init);
  REQUIRE(init.result()->status == hal::Status::timeout);
}

TEST_CASE("SD initialization validates voltage echo and power-up status") {
  Card card;
  sd::Initializer init{card.device()};
  card.bad_echo = true;
  REQUIRE(init.request() == hal::Status::ok);
  card.Run(init);
  REQUIRE(init.result()->status == hal::Status::response_mismatch);
  REQUIRE(init.result()->command == 8);
  card.bad_echo = false;
  card.powered_up = false;
  REQUIRE(init.request() == hal::Status::ok);
  card.Run(init);
  REQUIRE(init.result()->status == hal::Status::response_mismatch);
  REQUIRE(init.result()->command == 58);
}

TEST_CASE(
    "SD initialization retains buffers and propagates errors at every "
    "transfer") {
  for (std::size_t fail = 0; fail < 10; ++fail) {
    Card card;
    sd::Initializer init{card.device()};
    REQUIRE(init.request() == hal::Status::ok);
    std::size_t transfers = 0;
    for (std::size_t tick = 0; tick < 100 && !init.result(); ++tick) {
      init.tick();
      if (!card.pending.empty()) {
        for (int n = 0; n < 3; ++n) {
          init.tick();
        }
        REQUIRE_FALSE(init.result());
        REQUIRE(init.request() == hal::Status::busy);
        card.Complete(transfers++ == fail ? hal::Status::hardware_error
                                          : hal::Status::ok);
      }
    }
    REQUIRE(init.result());
    REQUIRE(init.result()->status == hal::Status::hardware_error);
    REQUIRE(transfers == fail + 1);
    REQUIRE(card.pending.empty());
    REQUIRE(init.request() == hal::Status::ok);
    card.Run(init);
    REQUIRE(init.result()->status == hal::Status::ok);
  }
}
