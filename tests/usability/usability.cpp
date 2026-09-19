#include "core/command/binding.hpp"
#include "core/command/command.hpp"
#include "support.hpp"

namespace core = daveos::core;
namespace test = testing;
namespace time_units = std::chrono;

TEST_CASE(
    "duration conversion is exact, bounded, and does not change failed "
    "output") {
  core::Time micros = 42;
  CHECK(core::to_microseconds(time_units::seconds{2}, micros) ==
        core::Status::ok);
  CHECK(micros == 2000000);
  CHECK(core::to_microseconds(time_units::nanoseconds{3000}, micros) ==
        core::Status::ok);
  CHECK(micros == 3);
  CHECK(core::to_microseconds(time_units::nanoseconds{3001}, micros) ==
        core::Status::invalid_argument);
  CHECK(micros == 3);
  CHECK(core::to_microseconds(time_units::microseconds{-1}, micros) ==
        core::Status::invalid_argument);
  CHECK(core::to_microseconds(time_units::hours{INT64_MAX}, micros) ==
        core::Status::invalid_argument);
  using Wide = time_units::duration<std::uint64_t, std::micro>;
  CHECK(core::to_microseconds(Wide{core::kForever}, micros) ==
        core::Status::invalid_argument);
  CHECK(micros == 3);
  CHECK(core::to_microseconds(Wide{core::kForever - 1}, micros) ==
        core::Status::ok);
  CHECK(micros == core::kForever - 1);
  CHECK(core::to_microseconds(time_units::milliseconds{0}, micros) ==
        core::Status::ok);
  CHECK(micros == 0);
  using HugePeriod = time_units::duration<std::uint64_t, std::ratio<INT64_MAX>>;
  CHECK(core::to_microseconds(HugePeriod{0}, micros) == core::Status::ok);
  CHECK(core::to_microseconds(HugePeriod{1}, micros) ==
        core::Status::invalid_argument);
}

TEST_CASE(
    "self scheduling validates tasks and preserves work after invalid "
    "durations") {
  struct Worker : core::Module<Worker, test::Event> {
    static constexpr const char* name() { return "worker"; }

    static constexpr auto tasks() {
      return std::array{DAVEOS_TASK(Worker, Work)};
    }

    core::Status init(core::InitStage stage) {
      if (stage == core::InitStage::stage1) {
        CHECK(schedule<&Worker::Work>(time_units::microseconds{5}) ==
              core::Status::ok);
        CHECK(cancel<&Worker::Work>() == core::Status::ok);
        CHECK(cancel<&Worker::Work>() == core::Status::not_found);
        CHECK(schedule<&Worker::Work>(time_units::microseconds{0},
                                      core::Mode::repeat) ==
              core::Status::invalid_argument);
        CHECK(schedule<&Worker::Work>(time_units::microseconds{5}) ==
              core::Status::ok);
        CHECK(schedule<&Worker::Work>(time_units::microseconds{-1}) ==
              core::Status::invalid_argument);
        CHECK(schedule<&Worker::Work>(time_units::nanoseconds{1}) ==
              core::Status::invalid_argument);
      }
      return core::Status::ok;
    }

    void Work() {
      ++calls;
      CHECK(scheduler().stop() == core::Status::ok);
    }

    int calls = 0;
  } worker;

  test::Fake platform;
  auto scheduler =
      core::make_scheduler<test::Event>(platform, core::ModuleList{&worker});
  CHECK(scheduler.run() == core::Status::ok);
  CHECK(worker.calls == 1);
  CHECK(platform.now() == 5);
  CHECK(std::string_view(scheduler.snapshot().tasks[0].task) == "Work");
}

TEST_CASE(
    "bound timers distinguish objects, replace at capacity, and rearm in "
    "interrupts") {
  struct Target {
    core::SchedulerInterface<test::Event>& scheduler;
    test::Fake& platform;
    int calls = 0;
    core::Time last = 0;

    void Fire() {
      CHECK(platform.in_interrupt());
      last = platform.now();
      if (++calls == 1) {
        CHECK(scheduler.timer<&Target::Fire>(
                  *this, time_units::microseconds{2}) == core::Status::ok);
      }
    }
  };

  test::Fake platform;
  test::TestModule module;
  auto scheduler = core::make_scheduler<test::Event, 32, 2>(
      platform, core::ModuleList{&module});
  Target first{scheduler, platform}, second{scheduler, platform},
      third{scheduler, platform};
  module.first_action = [&] {
    CHECK(scheduler.timer<&Target::Fire>(first, time_units::microseconds{20}) ==
          core::Status::ok);
    CHECK(scheduler.timer<&Target::Fire>(second, time_units::microseconds{6}) ==
          core::Status::ok);
    CHECK(scheduler.timer<&Target::Fire>(third, time_units::microseconds{1}) ==
          core::Status::full);
    CHECK(scheduler.timer<&Target::Fire>(first, time_units::microseconds{3}) ==
          core::Status::ok);
    CHECK(scheduler.timer<&Target::Fire>(first, time_units::nanoseconds{1}) ==
          core::Status::invalid_argument);
  };
  module.second_action = [&] { CHECK(scheduler.stop() == core::Status::ok); };
  CHECK(scheduler.timer<&Target::Fire>(first, time_units::microseconds{1}) ==
        core::Status::not_running);
  CHECK(scheduler.schedule(module, &test::TestModule::first,
                           time_units::microseconds{0}) == core::Status::ok);
  CHECK(scheduler.schedule<&test::TestModule::second>(
            module, time_units::microseconds{10}) == core::Status::ok);
  CHECK(scheduler.run() == core::Status::ok);
  CHECK(first.calls == 2);
  CHECK(first.last == 5);
  CHECK(second.calls == 2);
  CHECK(second.last == 8);
  CHECK(third.calls == 0);
}

TEST_CASE(
    "bound timer cancellation from a member ISR leaves other objects alone") {
  struct Target {
    core::SchedulerInterface<test::Event>& scheduler;
    Target* other = nullptr;
    int calls = 0;

    void Fire() {
      ++calls;
      CHECK(scheduler.cancel_timer<&Target::Fire>(*other) == core::Status::ok);
      CHECK(scheduler.cancel_timer<&Target::Fire>(*other) ==
            core::Status::not_found);
      CHECK(scheduler.stop() == core::Status::ok);
    }
  };

  test::Fake platform;
  test::TestModule module;
  auto scheduler =
      core::make_scheduler<test::Event>(platform, core::ModuleList{&module});
  Target first{scheduler}, second{scheduler};
  first.other = &second;
  module.first_action = [&] {
    CHECK(scheduler.timer<&Target::Fire>(first, 1) == core::Status::ok);
    CHECK(scheduler.timer<&Target::Fire>(second, 2) == core::Status::ok);
  };
  CHECK(scheduler.schedule(module, &test::TestModule::first, 0) ==
        core::Status::ok);
  CHECK(scheduler.run() == core::Status::ok);
  platform.advance(100);
  CHECK(first.calls == 1);
  CHECK(second.calls == 0);
}

TEST_CASE(
    "binding helper waits for stage2 and diagnoses a missing connection") {
  test::Fake platform;
  test::TestModule module;
  core::CommandSource source;
  auto binding =
      core::make_command_binding<test::Event>(core::CommandSourceList{source});
  auto modules = core::ModuleList{&binding, &module};
  auto scheduler = core::make_scheduler<test::Event>(platform, modules);
  core::CommandDispatcher dispatcher(modules, scheduler);
  bool stage1 = false, dispatched = false;
  module.initializer = [&](core::InitStage stage) {
    if (stage == core::InitStage::stage1) {
      CHECK(source.dispatch("help") == core::Status::not_running);
      stage1 = true;
      return scheduler.schedule(module, &test::TestModule::first, 0);
    }
    return core::Status::ok;
  };
  module.first_action = [&] {
    CHECK(stage1);
    CHECK(source.dispatch("help") == core::Status::ok);
    dispatched = true;
    CHECK(scheduler.stop() == core::Status::ok);
  };
  SECTION("connected") {
    binding.connect(dispatcher);
    CHECK(scheduler.run() == core::Status::ok);
    CHECK(dispatched);
    CHECK(scheduler.initialization_failure().status == core::Status::ok);
  }
  SECTION("not connected") {
    CHECK(scheduler.run() == core::Status::not_running);
    CHECK_FALSE(dispatched);
    const auto failure = scheduler.initialization_failure();
    CHECK(failure.status == core::Status::not_running);
    CHECK(std::string_view(failure.module) == "commands");
    CHECK(failure.stage == core::InitStage::stage2);
    CHECK(scheduler.init() == core::Status::not_running);
    CHECK(scheduler.initialization_failure().module == failure.module);
  }
}

TEST_CASE(
    "bound callback identity normalizes base references and supports const "
    "noexcept") {
  struct Base {
    int calls = 0;

    void Fire() noexcept { ++calls; }
  };

  struct Other {
    int padding = 1;
  };

  struct Derived : Other, Base {
  } object;

  Base& base = object;
  auto first = core::TimerCallback::bind<&Base::Fire>(base);
  auto second = core::TimerCallback::bind<&Base::Fire>(object);
  CHECK(first == second);
  first();
  second();
  CHECK(object.calls == 2);

  struct ConstTarget {
    mutable int calls = 0;

    void Fire() const noexcept { ++calls; }
  };

  const ConstTarget target;
  auto callback = core::TimerCallback::bind<&ConstTarget::Fire>(target);
  callback();
  CHECK(target.calls == 1);
  CHECK_FALSE(core::TimerCallback{});
}
