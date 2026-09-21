#include <thread>

#include "hal/adapters/daveos.hpp"
#include "hal/controller.hpp"
#include "hal/registry.hpp"
#include "platform/fake/bus.hpp"
#include "support.hpp"

namespace {

  namespace hal = daveos::hal;
  namespace spi = hal::spi;
  namespace i2c = hal::i2c;
  namespace fake = daveos::platform::fake;
  namespace chrono = std::chrono;

  struct Fixture {
    testing::Fake platform;
    fake::BusClock<> clock;
    fake::BusCritical critical;
    fake::SpiBus backend;
    hal::Controller<fake::SpiBus, fake::BusClock<>, fake::BusCritical, 2> bus{
        backend, clock, critical, {{{1, 100000}, {2, 1000000}}}};

    Fixture() { REQUIRE(bus.init() == hal::Status::ok); }

    void pump() {
      std::size_t calls = 0;
      while (backend.take_pending()) {
        REQUIRE(++calls < 100);
        REQUIRE(platform.interrupt(
                    [](void* self) {
                      static_cast<Fixture*>(self)->bus.interrupt();
                    },
                    this) == testing::core::Status::ok);
      }
    }

    void advance(std::uint64_t us) {
      clock.advance(us);
      while (auto callback = clock.take_due()) {
        REQUIRE(platform.interrupt(
                    [](void* p) {
                      (*static_cast<daveos::core::TimerCallback*>(p))();
                    },
                    &callback) == testing::core::Status::ok);
      }
      pump();
    }
  };
}  // namespace

TEST_CASE(
    "HAL validates chrono and complete action lists before touching the bus") {
  Fixture f;
  std::array<std::uint8_t, 3> bytes{};
  const std::array actions{spi::write(bytes),
                           spi::pause(chrono::microseconds{0})};
  spi::Completion done;
  REQUIRE(done.start(f.bus.device<0>(), actions) ==
          hal::Status::invalid_argument);
  REQUIRE(done.ready());
  REQUIRE(done.result()->actions.data() == actions.data());
  REQUIRE(f.backend.begins == 0);
  REQUIRE(f.clock.pending() == 0);
  REQUIRE_FALSE(hal::Duration(chrono::nanoseconds{1}).valid());
  REQUIRE_FALSE(hal::Duration(chrono::milliseconds{-1}).valid());
  REQUIRE(hal::Duration(chrono::milliseconds{1}).microseconds() == 1000);
  const std::array overlap{spi::exchange(bytes, bytes)};
  REQUIRE(done.start(f.bus.device<0>(), overlap) ==
          hal::Status::invalid_argument);
}

TEST_CASE("SPI executes borrowed actions across a CS-held pause") {
  Fixture f;
  std::array<std::uint8_t, 2> tx{1, 2}, rx{}, answer{7, 8};
  const std::array actions{spi::write(tx), spi::pause(chrono::microseconds{25}),
                           spi::read(rx)};
  const std::array script{fake::SpiBus::Step{actions[0]},
                          fake::SpiBus::Step{actions[2], answer}};
  f.backend.script(script);
  spi::Completion done;
  REQUIRE(done.start(f.bus.device<0>(), actions) == hal::Status::ok);
  REQUIRE_FALSE(done.ready());
  f.pump();
  REQUIRE(f.backend.selected);
  REQUIRE(f.clock.pending() == 2);
  REQUIRE(done.start(f.bus.device<1>(), actions) == hal::Status::busy);
  f.advance(24);
  REQUIRE_FALSE(done.ready());
  f.advance(1);
  REQUIRE(done.ready());
  REQUIRE(done.result()->status == hal::Status::ok);
  REQUIRE(done.result()->completed_actions == 3);
  REQUIRE(rx == answer);
  REQUIRE_FALSE(f.backend.selected);
  REQUIRE(f.clock.pending() == 0);
  REQUIRE(f.bus.statistics().completed == 1);
  REQUIRE(f.bus.statistics().read_attempts == 1);
  REQUIRE(f.bus.statistics().write_attempts == 1);
}

TEST_CASE(
    "Timeout returns storage despite failed cleanup and reset restores the "
    "bus") {
  Fixture f;
  std::array<std::uint8_t, 1> tx{1};
  const std::array actions{spi::write(tx)};
  const std::array script{
      fake::SpiBus::Step{actions[0], {}, hal::Status::ok, true}};
  f.backend.script(script);
  f.backend.cleanup_result = hal::Status::hardware_error;
  spi::Completion done;
  REQUIRE(done.start(f.bus.device<0>(), actions, chrono::microseconds{10}) ==
          hal::Status::ok);
  f.pump();
  f.advance(10);
  REQUIRE(done.result()->status == hal::Status::timeout);
  REQUIRE(f.bus.faulted());
  REQUIRE(f.bus.statistics().cleanup_failures == 1);
  REQUIRE(f.bus.statistics().write_errors == 1);
  REQUIRE(done.start(f.bus.device<0>(), actions) == hal::Status::faulted);
  hal::Status reset = hal::Status::busy;
  REQUIRE(f.bus.reset({&reset, [](void* p, const hal::ResetResult& r) {
                         *static_cast<hal::Status*>(p) = r.status;
                       }}) == hal::Status::ok);
  f.pump();
  REQUIRE(reset == hal::Status::ok);
  REQUIRE_FALSE(f.bus.faulted());
}

TEST_CASE("Alarm failure rejects before CS and pause failure completes once") {
  Fixture f;
  std::array<std::uint8_t, 1> tx{1};
  const std::array actions{spi::write(tx), spi::pause(chrono::microseconds{1})};
  const std::array script{fake::SpiBus::Step{actions[0]}};
  f.backend.script(script);
  spi::Completion done;
  f.clock.fail_arm = true;
  REQUIRE(done.start(f.bus.device<0>(), actions) == hal::Status::timer_error);
  REQUIRE(f.backend.begins == 0);
  f.clock.fail_arm = false;
  REQUIRE(done.start(f.bus.device<0>(), actions) == hal::Status::ok);
  f.clock.fail_arm = true;
  f.pump();
  REQUIRE(done.result()->status == hal::Status::timer_error);
  REQUIRE(done.result()->completed_actions == 1);
  REQUIRE(f.bus.statistics().completed == 1);
  REQUIRE(f.clock.pending() == 0);
}

TEST_CASE("Device and controller statistics clear independently") {
  Fixture f;
  const std::array actions{spi::idle_clocks(80)};
  const std::array script{fake::SpiBus::Step{actions[0]}};
  f.backend.script(script);
  spi::Completion done;
  REQUIRE(done.start(f.bus.device<0>(), actions) == hal::Status::ok);
  f.pump();
  REQUIRE(done.result()->status == hal::Status::ok);
  REQUIRE_FALSE(f.backend.trace[0].selected);
  REQUIRE(f.bus.statistics().write_attempts == 0);
  f.bus.device<0>().clear_statistics();
  REQUIRE(f.bus.device<0>().statistics().completed == 0);
  REQUIRE(f.bus.statistics().completed == 1);
  f.bus.clear_statistics();
  REQUIRE(f.bus.statistics().completed == 0);
}

TEST_CASE(
    "Callback can reuse descriptors and immediately chain on the same "
    "controller") {
  Fixture f;
  std::array<std::uint8_t, 1> tx{1};
  std::array actions{spi::write(tx)};
  const std::array script{fake::SpiBus::Step{actions[0]},
                          fake::SpiBus::Step{actions[0]}};
  f.backend.script(script);

  struct Chain {
    Fixture& f;
    std::span<const spi::Action> actions;
    std::size_t calls = 0;

    void Done(const spi::Result& result) {
      REQUIRE(f.platform.in_interrupt());
      REQUIRE_FALSE(f.backend.selected);
      REQUIRE(result.actions.data() == actions.data());
      REQUIRE(result.status == hal::Status::ok);
      ++calls;
      if (calls == 1) {
        REQUIRE(f.bus.device<0>().start(
                    actions, spi::Callback::bind<&Chain::Done>(*this)) ==
                hal::Status::ok);
      }
    }
  } chain{f, actions};

  REQUIRE(f.bus.device<0>().start(actions, spi::Callback::bind<&Chain::Done>(
                                               chain)) == hal::Status::ok);
  f.pump();
  REQUIRE(chain.calls == 2);
  REQUIRE(f.bus.statistics().completed == 2);
}

TEST_CASE(
    "Already selected alarm cannot prematurely complete a replacement "
    "request") {
  Fixture f;
  std::array<std::uint8_t, 1> tx{1};
  const std::array actions{spi::write(tx)};
  const std::array script{
      fake::SpiBus::Step{actions[0], {}, hal::Status::ok, true}};
  f.backend.script(script);
  spi::Completion done;
  REQUIRE(done.start(f.bus.device<0>(), actions, chrono::microseconds{10}) ==
          hal::Status::ok);
  f.pump();
  f.clock.advance(10);
  auto stale = f.clock.take_due();
  REQUIRE(bool(stale));
  // A peripheral IRQ services the deadline before the selected timer runs.
  REQUIRE(f.platform.interrupt(
              [](void* p) { static_cast<Fixture*>(p)->bus.interrupt(); }, &f) ==
          testing::core::Status::ok);
  REQUIRE(done.result()->status == hal::Status::timeout);
  f.backend.script(script);
  REQUIRE(done.start(f.bus.device<0>(), actions, chrono::microseconds{100}) ==
          hal::Status::ok);
  REQUIRE(
      f.platform.interrupt(
          [](void* p) { (*static_cast<daveos::core::TimerCallback*>(p))(); },
          &stale) == testing::core::Status::ok);
  REQUIRE_FALSE(done.ready());
  f.advance(100);
  REQUIRE(done.result()->status == hal::Status::timeout);
  REQUIRE(f.bus.statistics().completed == 2);
}

TEST_CASE(
    "I2C raw phases preserve repeated-start framing and stop at the first "
    "error") {
  testing::Fake platform;
  fake::BusClock<> clock;
  fake::BusCritical critical;
  fake::I2cBus backend;
  hal::Controller<fake::I2cBus, fake::BusClock<>, fake::BusCritical, 1> bus{
      backend, clock, critical, {{{{0x3c}, 100000}}}};
  REQUIRE(bus.init() == hal::Status::ok);
  std::array<std::uint8_t, 1> tx{4}, rx{};
  const std::array actions{i2c::write(tx), i2c::write(tx), i2c::read(rx)};
  const std::array script{
      fake::I2cBus::Step{actions[0]},
      fake::I2cBus::Step{actions[1], {}, hal::Status::nack}};
  backend.script(script);
  i2c::Completion done;
  REQUIRE(done.start(bus.device<0>(), actions) == hal::Status::ok);
  while (backend.take_pending()) {
    REQUIRE(platform.interrupt(
                [](void* p) { static_cast<decltype(bus)*>(p)->interrupt(); },
                &bus) == testing::core::Status::ok);
  }
  REQUIRE(done.result()->status == hal::Status::nack);
  REQUIRE(done.result()->completed_actions == 1);
  REQUIRE(backend.trace_count == 2);
  REQUIRE(backend.trace[0].first);
  REQUIRE_FALSE(backend.trace[1].first);
  REQUIRE_FALSE(backend.trace[1].last);
  REQUIRE(bus.statistics().read_attempts == 0);
  REQUIRE(bus.statistics().write_attempts == 2);
  REQUIRE(bus.statistics().write_errors == 1);
  REQUIRE(i2c::Address{0x08}.valid());
  REQUIRE(i2c::Address{0x77}.valid());
  REQUIRE_FALSE(i2c::Address{0x07}.valid());
  REQUIRE_FALSE(i2c::Address{0x78}.valid());
}

TEST_CASE(
    "Setup failures complete asynchronously without launching an action") {
  Fixture f;
  std::array<std::uint8_t, 1> tx{};
  const std::array actions{spi::write(tx)};
  f.backend.begin_result = hal::Status::hardware_error;
  spi::Completion done;
  REQUIRE(done.start(f.bus.device<0>(), actions) == hal::Status::ok);
  REQUIRE_FALSE(done.ready());
  f.pump();
  REQUIRE(done.result()->status == hal::Status::hardware_error);
  REQUIRE(f.backend.trace_count == 0);
  REQUIRE(f.bus.statistics().write_attempts == 0);
}

TEST_CASE("Automatic timeout uses effective rate and explicit pause duration") {
  std::array<std::uint8_t, 1000> tx{};
  const std::array actions{spi::write(tx),
                           spi::pause(chrono::milliseconds{20})};
  std::uint64_t timeout = 0;
  REQUIRE(hal::detail::Timeout<spi::Action>(actions, 1000000, {}, timeout) ==
          hal::Status::ok);
  REQUIRE(timeout == 53000);
  REQUIRE(hal::detail::Timeout<spi::Action>(actions, 1000000,
                                            chrono::microseconds{1},
                                            timeout) == hal::Status::ok);
  REQUIRE(timeout == 1);
  const std::array huge{spi::idle_clocks(daveos::core::kForever - 7)};
  REQUIRE(hal::detail::Timeout<spi::Action>(huge, 1, {}, timeout) ==
          hal::Status::invalid_argument);
}

TEST_CASE(
    "Bus registry validates all entries and rolls back partial "
    "initialization") {
  fake::BusClock<> clock;
  fake::BusCritical ca, cb;
  fake::SpiBus a, b;
  using Bus =
      hal::Controller<fake::SpiBus, fake::BusClock<>, fake::BusCritical, 1>;
  Bus first{a, clock, ca, {{{1, 100000}}}},
      second{b, clock, cb, {{{2, 100000}}}};
  enum class Alias { a, b };
  hal::BusRegistry buses{hal::bus<Alias::a>(first), hal::bus<Alias::b>(second)};
  b.initialization_result = hal::Status::initialization_failed;
  REQUIRE(buses.init() == hal::Status::initialization_failed);
  REQUIRE(buses.initialization_failure()->entry == 1);
  REQUIRE(a.disabled);
  REQUIRE(b.disabled);
  REQUIRE(&buses.controller<Alias::a>() == &first);
  hal::Registry<spi::Action, Alias::a, Alias::b> devices{
      {first.device<0>(), second.device<0>()}};
  spi::Completion done;
  const std::array actions{spi::idle_clocks(80)};
  REQUIRE(done.start(devices.device<Alias::a>(), actions) ==
          hal::Status::not_initialized);
}

TEST_CASE(
    "Registry rejects shared CS pins before either controller touches "
    "hardware") {
  fake::BusClock<> clock;
  fake::BusCritical ca, cb;
  fake::SpiBus a, b;
  using Bus =
      hal::Controller<fake::SpiBus, fake::BusClock<>, fake::BusCritical, 1>;
  Bus first{a, clock, ca, {{{1, 100000}}}},
      second{b, clock, cb, {{{1, 100000}}}};
  enum class Alias { a, b };
  hal::BusRegistry buses{hal::bus<Alias::a>(first), hal::bus<Alias::b>(second)};
  REQUIRE(buses.init() == hal::Status::invalid_argument);
  REQUIRE(buses.initialization_failure()->entry == 1);
  REQUIRE_FALSE(a.disabled);
  REQUIRE_FALSE(b.disabled);
}

TEST_CASE("Publication helper preserves completion before start returns") {
  const std::array actions{spi::idle_clocks(80)};

  struct Immediate {
    hal::Status start(std::span<const spi::Action> a, spi::Callback callback,
                      std::optional<hal::Duration>) const {
      callback({hal::Status::ok, a.size(), a});
      return hal::Status::ok;
    }
  } immediate;

  spi::Completion completion;
  REQUIRE(completion.start(immediate, actions) == hal::Status::ok);
  REQUIRE(completion.ready());
  REQUIRE(completion.result()->completed_actions == 1);
}

TEST_CASE(
    "Busy controller leaves another device request untouched and independent "
    "buses progress") {
  Fixture a, b;
  std::array<std::uint8_t, 1> tx{};
  const std::array actions{spi::write(tx)};
  const std::array hang{
      fake::SpiBus::Step{actions[0], {}, hal::Status::ok, true}};
  const std::array success{fake::SpiBus::Step{actions[0]}};
  a.backend.script(hang);
  b.backend.script(success);
  spi::Completion ca, cb, rejected;
  REQUIRE(ca.start(a.bus.device<0>(), actions, chrono::milliseconds{1}) ==
          hal::Status::ok);
  REQUIRE(rejected.start(a.bus.device<1>(), actions) == hal::Status::busy);
  REQUIRE(cb.start(b.bus.device<0>(), actions) == hal::Status::ok);
  a.pump();
  b.pump();
  REQUIRE(cb.result()->status == hal::Status::ok);
  REQUIRE_FALSE(ca.ready());
  REQUIRE(a.bus.device<1>().statistics().rejected == 1);
  a.advance(1000);
  REQUIRE(ca.result()->status == hal::Status::timeout);
}

TEST_CASE(
    "HAL lock contention is nonblocking and counts the rejected request") {
  Fixture f;
  const std::array actions{spi::idle_clocks(80)};
  spi::Completion done;
  std::atomic<bool> locked = false, release = false;
  std::thread holder([&] {
    f.critical.enter();
    locked.store(true, std::memory_order_release);
    while (!release.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    f.critical.leave();
  });
  while (!locked.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  const auto result = done.start(f.bus.device<0>(), actions);
  release.store(true, std::memory_order_release);
  holder.join();
  REQUIRE(result == hal::Status::busy);
  REQUIRE(done.result()->status == hal::Status::busy);
  REQUIRE(f.bus.statistics().rejected == 1);
  REQUIRE(f.backend.begins == 0);
}

TEST_CASE("ISR completion publication is visible to a polling host task") {
  Fixture f;
  std::array<std::uint8_t, 4> rx{}, answer{2, 3, 5, 7};
  const std::array actions{spi::read(rx)};
  const std::array script{fake::SpiBus::Step{actions[0], answer}};
  f.backend.script(script);
  spi::Completion done;
  REQUIRE(done.start(f.bus.device<0>(), actions) == hal::Status::ok);
  std::thread interrupt([&] {
    while (f.backend.take_pending()) {
      f.platform.interrupt(
          [](void* p) { static_cast<Fixture*>(p)->bus.interrupt(); }, &f);
    }
  });
  while (!done.ready()) {
    std::this_thread::yield();
  }
  const auto result = done.result();
  const auto observed = rx;
  interrupt.join();
  REQUIRE(result->status == hal::Status::ok);
  REQUIRE(observed == answer);
}

TEST_CASE(
    "DaveOS timer adapter rejects init-time transfers and handles a real "
    "scheduler deadline") {
  testing::Fake platform;
  testing::TestModule module;
  auto scheduler = testing::core::make_scheduler<testing::Event>(
      platform, testing::core::ModuleList{&module});
  hal::DaveOsClock clock{scheduler, platform};
  fake::BusCritical critical;
  fake::SpiBus backend;
  hal::Controller<fake::SpiBus, decltype(clock), fake::BusCritical, 1> bus{
      backend, clock, critical, {{{1, 100000}}}};
  REQUIRE(bus.init() == hal::Status::ok);
  std::array<std::uint8_t, 1> tx{};
  const std::array actions{spi::write(tx)};
  const std::array script{
      fake::SpiBus::Step{actions[0], {}, hal::Status::ok, true}};
  backend.script(script);
  spi::Completion completion;
  REQUIRE(completion.start(bus.device<0>(), actions) ==
          hal::Status::timer_error);
  REQUIRE(backend.begins == 0);
  module.first_action = [&] {
    REQUIRE(completion.start(bus.device<0>(), actions,
                             chrono::microseconds{100}) == hal::Status::ok);
    platform.interrupt(
        [](void* p) { static_cast<decltype(bus)*>(p)->interrupt(); }, &bus);
  };
  module.second_action = [&] {
    CHECK(completion.ready());
    CHECK(completion.result()->status == hal::Status::timeout);
    scheduler.stop();
  };
  scheduler.schedule(module, &testing::TestModule::first, 0);
  scheduler.schedule(module, &testing::TestModule::second, 101);
  REQUIRE(scheduler.run() == testing::core::Status::ok);
  REQUIRE(bus.statistics().timed_out == 1);
}

TEST_CASE("Unselected SPI clocks reject mixed lists and altered fill bytes") {
  Fixture f;
  spi::Completion completion;
  const std::array mixed{spi::idle_clocks(80),
                         spi::pause(chrono::microseconds{1})};
  REQUIRE(completion.start(f.bus.device<0>(), mixed) ==
          hal::Status::invalid_argument);
  std::array action{spi::idle_clocks(80)};
  action[0].fill = 0;
  REQUIRE(completion.start(f.bus.device<0>(), action) ==
          hal::Status::invalid_argument);
  action[0] = spi::idle_clocks(7);
  REQUIRE(completion.start(f.bus.device<0>(), action) ==
          hal::Status::invalid_argument);
  REQUIRE(f.backend.begins == 0);
}

TEST_CASE(
    "Independent controllers can share a fake alarm pool across host threads") {
  fake::BusClock<2> clock;

  struct Target {
    void Fire() {}
  } a, b;

  const auto ca = daveos::core::TimerCallback::bind<&Target::Fire>(a);
  const auto cb = daveos::core::TimerCallback::bind<&Target::Fire>(b);
  std::atomic<bool> invalid = false;
  auto exercise = [&](const auto& callback) {
    for (std::size_t i = 0; i < 1000; ++i) {
      const auto status = clock.arm(100, callback);
      if (status != hal::Status::ok && status != hal::Status::timer_error) {
        invalid = true;
      }
      clock.cancel(callback);
    }
  };
  std::thread first([&] { exercise(ca); });
  std::thread second([&] { exercise(cb); });
  first.join();
  second.join();
  REQUIRE_FALSE(invalid);
  REQUIRE(clock.pending() == 0);
}

TEST_CASE("SPI response checks gate following writes without releasing CS") {
  for (const auto response :
       {std::uint8_t{0}, std::uint8_t{4}, std::uint8_t{0xff}}) {
    Fixture f;
    std::array<std::uint8_t, 3> input{}, reply{0xff, response, 0xff};
    const std::array<std::uint8_t, 1> data{42};
    const std::array actions{spi::read(input), spi::check_response(input, 0),
                             spi::write(data)};
    const std::array script{fake::SpiBus::Step{spi::read(input), reply},
                            fake::SpiBus::Step{spi::write(data)}};
    f.backend.script(script);
    spi::Completion completion;
    REQUIRE(completion.start(f.bus.device<0>(), actions) == hal::Status::ok);
    f.pump();
    REQUIRE(completion.ready());
    CHECK(completion.result()->status ==
          (response == 0 ? hal::Status::ok : hal::Status::response_mismatch));
    CHECK(completion.result()->completed_actions == (response == 0 ? 3 : 1));
    CHECK_FALSE(f.backend.selected);
  }
}

TEST_CASE(
    "SPI rejects unbounded or malformed response checks before admission") {
  Fixture f;
  std::array<std::uint8_t, 33> bytes{};
  for (const auto check :
       {spi::check_response({}, 0), spi::check_response(bytes, 0),
        spi::check_response(std::span{bytes}.first(1), 0, 0),
        spi::check_response(std::span{bytes}.first(1), 0x80, 0x1f)}) {
    spi::Completion completion;
    const std::array actions{check};
    CHECK(completion.start(f.bus.device<0>(), actions) ==
          hal::Status::invalid_argument);
  }
  CHECK(f.backend.begins == 0);
}
