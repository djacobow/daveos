#include <semaphore>
#include <thread>

#include "support.hpp"
using namespace testing;

TEST_CASE("timer replacement, cancellation, capacity and self-rearm") {
  Fake platform;
  TestModule module;
  auto scheduler = make_scheduler<Event, 32, 1>(platform, ModuleList{&module});
  int calls = 0;
  std::vector<Time> times;
  timer_action = [&] {
    CHECK(platform.in_interrupt());
    times.push_back(platform.now());
    if (++calls == 1)
      CHECK(scheduler.timer(4, Timer) == Status::ok);
    else
      scheduler.schedule(module, &TestModule::second, 0);
  };
  module.first_action = [&] {
    CHECK(scheduler.timer(0, Timer) == Status::invalid_argument);
    CHECK(scheduler.timer(20, Timer) == Status::ok);
    CHECK(scheduler.timer(5, Timer) == Status::ok);
    CHECK(scheduler.timer(1, OtherTimer) == Status::full);
    CHECK(scheduler.cancel_timer(OtherTimer) == Status::not_found);
  };
  module.second_action = [&] {
    CHECK_FALSE(platform.in_interrupt());
    scheduler.stop();
  };
  CHECK(scheduler.timer(1, Timer) == Status::not_running);
  scheduler.schedule(module, &TestModule::first, 0);
  CHECK(scheduler.run() == Status::ok);
  CHECK(times == std::vector<Time>{5, 9});
  CHECK(scheduler.snapshot().timer_overflows == 1);
  CHECK(platform.sleeps() > 0);
}

TEST_CASE(
    "timer callback can cancel another timer and shutdown discards remaining "
    "timers") {
  Fake platform;
  TestModule module;
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module});
  int unwanted = 0;
  other_timer_action = [&] { ++unwanted; };
  timer_action = [&] {
    CHECK(scheduler.cancel_timer(OtherTimer) == Status::ok);
    CHECK(scheduler.cancel_timer(OtherTimer) == Status::not_found);
    scheduler.timer(10, OtherTimer);
    scheduler.stop();
  };
  module.first_action = [&] {
    scheduler.timer(5, Timer);
    scheduler.timer(10, OtherTimer);
  };
  scheduler.schedule(module, &TestModule::first, 0);
  CHECK(scheduler.run() == Status::ok);
  platform.advance(100);
  CHECK(unwanted == 0);
}

TEST_CASE(
    "fake advance delivers platform callbacks synchronously at deadlines") {
  Fake platform;
  struct State {
    Fake* platform;
    int count = 0;
    Time fired = 0;
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
  Fake platform(Fake::Advancement::manual);
  TestModule module;
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module});
  module.first_action = [&] { scheduler.stop(); };
  scheduler.schedule(module, &TestModule::first, 10);
  std::thread runner([&] { scheduler.run(); });
  while (!platform.sleeps()) std::this_thread::yield();
  platform.advance(10);
  runner.join();
  CHECK(platform.now() == 10);
}

TEST_CASE("indefinite fake sleep wakes on an interrupt") {
  Fake platform;
  TestModule module;
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module});
  module.first_action = [&] { scheduler.stop(); };
  std::thread runner([&] { scheduler.run(); });
  while (!platform.sleeps()) std::this_thread::yield();
  platform.interrupt(
      [](void* context) {
        auto& module = *static_cast<TestModule*>(context);
        module.scheduler().schedule(module, &TestModule::first, 0);
      },
      &module);
  runner.join();
  CHECK(platform.now() == 0);
}

TEST_CASE("can_sleep false takes awake-wait path") {
  Fake platform;
  TestModule module;
  module.sleep = false;
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module});
  module.first_action = [&] { scheduler.stop(); };
  scheduler.schedule(module, &TestModule::first, 10);
  CHECK(scheduler.run() == Status::ok);
  CHECK(platform.sleeps() == 0);
  CHECK(platform.awake_waits() > 0);
}

TEST_CASE(
    "time spent in a fake timer callback does not nest interrupts or lose "
    "another expiry") {
  Fake platform;
  TestModule module;
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module});
  int depth = 0, maximum = 0, calls = 0;
  timer_action = [&] {
    maximum = std::max(maximum, ++depth);
    platform.advance(10);
    --depth;
  };
  other_timer_action = [&] {
    maximum = std::max(maximum, ++depth);
    ++calls;
    --depth;
    scheduler.stop();
  };
  module.first_action = [&] {
    scheduler.timer(5, Timer);
    scheduler.timer(8, OtherTimer);
  };
  scheduler.schedule(module, &TestModule::first, 0);
  CHECK(scheduler.run() == Status::ok);
  CHECK(calls == 1);
  CHECK(maximum == 1);
}
