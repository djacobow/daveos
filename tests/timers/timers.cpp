#include <semaphore>
#include <thread>

#include "support.hpp"

namespace core = daveos::core;
namespace test = testing;

TEST_CASE("timer replacement, cancellation, capacity and self-rearm") {
  test::Fake platform;
  test::TestModule module;
  auto scheduler = core::make_scheduler<test::Event, 32, 1>(
      platform, core::ModuleList{&module});
  int calls = 0;
  std::vector<core::Time> times;
  test::timer_action = [&] {
    CHECK(platform.in_interrupt());
    times.push_back(platform.now());
    if (++calls == 1) {
      CHECK(scheduler.timer(4, test::Timer) == core::Status::ok);
    } else {
      scheduler.schedule(module, &test::TestModule::second, 0);
    }
  };
  module.first_action = [&] {
    CHECK(scheduler.timer(0, test::Timer) == core::Status::invalid_argument);
    CHECK(scheduler.timer(20, test::Timer) == core::Status::ok);
    CHECK(scheduler.timer(5, test::Timer) == core::Status::ok);
    CHECK(scheduler.timer(1, test::OtherTimer) == core::Status::full);
    CHECK(scheduler.cancel_timer(test::OtherTimer) == core::Status::not_found);
  };
  module.second_action = [&] {
    CHECK_FALSE(platform.in_interrupt());
    scheduler.stop();
  };
  CHECK(scheduler.timer(1, test::Timer) == core::Status::not_running);
  scheduler.schedule(module, &test::TestModule::first, 0);
  CHECK(scheduler.run() == core::Status::ok);
  CHECK(times == std::vector<core::Time>{5, 9});
  CHECK(scheduler.snapshot().timer_overflows == 1);
  CHECK(platform.sleeps() > 0);
}

TEST_CASE(
    "timer callback can cancel another timer and shutdown discards remaining "
    "timers") {
  test::Fake platform;
  test::TestModule module;
  auto scheduler =
      core::make_scheduler<test::Event>(platform, core::ModuleList{&module});
  int unwanted = 0;
  test::other_timer_action = [&] { ++unwanted; };
  test::timer_action = [&] {
    CHECK(scheduler.cancel_timer(test::OtherTimer) == core::Status::ok);
    CHECK(scheduler.cancel_timer(test::OtherTimer) == core::Status::not_found);
    scheduler.timer(10, test::OtherTimer);
    scheduler.stop();
  };
  module.first_action = [&] {
    scheduler.timer(5, test::Timer);
    scheduler.timer(10, test::OtherTimer);
  };
  scheduler.schedule(module, &test::TestModule::first, 0);
  CHECK(scheduler.run() == core::Status::ok);
  platform.advance(100);
  CHECK(unwanted == 0);
}

TEST_CASE(
    "fake advance delivers platform callbacks synchronously at deadlines") {
  test::Fake platform;

  struct State {
    test::Fake* platform;
    int count = 0;
    core::Time fired = 0;
  } state{&platform};

  platform.arm(
      7,
      [](void* context) {
        auto& state = *static_cast<State*>(context);
        CHECK(state.platform->in_interrupt());
        state.fired = state.platform->now();
        ++state.count;
      },
      &state);
  platform.advance(20);
  CHECK(state.count == 1);
  CHECK(state.fired == 7);
  CHECK(platform.now() == 20);
}

TEST_CASE("manual fake sleep is released by time advancement") {
  test::Fake platform(test::Fake::Advancement::manual);
  test::TestModule module;
  auto scheduler =
      core::make_scheduler<test::Event>(platform, core::ModuleList{&module});
  module.first_action = [&] { scheduler.stop(); };
  scheduler.schedule(module, &test::TestModule::first, 10);
  std::thread runner([&] { scheduler.run(); });
  while (!platform.sleeps()) {
    std::this_thread::yield();
  }
  platform.advance(10);
  runner.join();
  CHECK(platform.now() == 10);
}

TEST_CASE("indefinite fake sleep wakes on an interrupt") {
  test::Fake platform;
  test::TestModule module;
  auto scheduler =
      core::make_scheduler<test::Event>(platform, core::ModuleList{&module});
  module.first_action = [&] { scheduler.stop(); };
  std::thread runner([&] { scheduler.run(); });
  while (!platform.sleeps()) {
    std::this_thread::yield();
  }
  platform.interrupt(
      [](void* context) {
        auto& module = *static_cast<test::TestModule*>(context);
        module.scheduler().schedule(module, &test::TestModule::first, 0);
      },
      &module);
  runner.join();
  CHECK(platform.now() == 0);
}

TEST_CASE("can_sleep false takes awake-wait path") {
  test::Fake platform;
  test::TestModule module;
  module.sleep = false;
  auto scheduler =
      core::make_scheduler<test::Event>(platform, core::ModuleList{&module});
  module.first_action = [&] { scheduler.stop(); };
  scheduler.schedule(module, &test::TestModule::first, 10);
  CHECK(scheduler.run() == core::Status::ok);
  CHECK(platform.sleeps() == 0);
  CHECK(platform.awake_waits() > 0);
}

TEST_CASE(
    "time spent in a fake timer callback does not nest interrupts or lose "
    "another expiry") {
  test::Fake platform;
  test::TestModule module;
  auto scheduler =
      core::make_scheduler<test::Event>(platform, core::ModuleList{&module});
  int depth = 0, maximum = 0, calls = 0;
  test::timer_action = [&] {
    maximum = std::max(maximum, ++depth);
    platform.advance(10);
    --depth;
  };
  test::other_timer_action = [&] {
    maximum = std::max(maximum, ++depth);
    ++calls;
    --depth;
    scheduler.stop();
  };
  module.first_action = [&] {
    scheduler.timer(5, test::Timer);
    scheduler.timer(8, test::OtherTimer);
  };
  scheduler.schedule(module, &test::TestModule::first, 0);
  CHECK(scheduler.run() == core::Status::ok);
  CHECK(calls == 1);
  CHECK(maximum == 1);
}
