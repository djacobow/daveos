#include <thread>

#include "core/schedule/application.hpp"
#include "support.hpp"

namespace core = daveos::core;
namespace test = testing;

TEST_CASE(
    "yield runs one earliest eligible task and restores context and "
    "accounting") {
  test::Fake platform;
  test::TestModule m;
  auto scheduler =
      core::make_scheduler<test::Event>(platform, core::ModuleList{&m});
  std::vector<int> order;
  m.first_action = [&] {
    order.push_back(1);
    platform.advance(10);
    CHECK(scheduler.yield() == core::Status::ok);
    CHECK(order == std::vector{1, 2});
    CHECK(std::string_view{platform.context().task} == "first");
    CHECK(platform.context().kind == core::CallbackKind::task);
    CHECK(platform.context().scheduler == &scheduler);
    platform.advance(2);
    CHECK(scheduler.yield() == core::Status::ok);
    CHECK(order == std::vector{1, 2, 3});
    CHECK(scheduler.yield() == core::Status::empty);
    scheduler.stop();
  };
  m.second_action = [&] {
    order.push_back(2);
    platform.advance(4);
  };
  m.third_action = [&] {
    order.push_back(3);
    platform.advance(3);
  };
  scheduler.schedule(m, &test::TestModule::first, 0);
  scheduler.schedule(m, &test::TestModule::third, 2);
  scheduler.schedule(m, &test::TestModule::second, 1);
  CHECK(scheduler.run() == core::Status::ok);
  const auto stats = scheduler.snapshot();
  CHECK(stats.tasks[0].total_duration == 19);
  CHECK(stats.tasks[0].total_nested_duration == 7);
  CHECK(stats.tasks[0].total_self_duration == 12);
  CHECK(stats.tasks[1].total_duration == 4);
  CHECK(stats.tasks[2].total_duration == 3);
  CHECK(platform.context().kind == core::CallbackKind::outside);
}

TEST_CASE("yield excludes active callbacks even after explicit rescheduling") {
  test::Fake platform;
  test::TestModule m;
  auto scheduler =
      core::make_scheduler<test::Event>(platform, core::ModuleList{&m});
  std::uint32_t calls = 0;
  m.first_action = [&] {
    ++calls;
    if (calls == 1) {
      scheduler.schedule(m, &test::TestModule::first, 0);
      CHECK(scheduler.yield() == core::Status::ok);
      CHECK(calls == 1);
    } else {
      scheduler.stop();
    }
  };
  m.second_action = [&] { CHECK(scheduler.yield() == core::Status::empty); };
  scheduler.schedule(m, &test::TestModule::first, 0);
  scheduler.schedule(m, &test::TestModule::second, 0);
  CHECK(scheduler.run() == core::Status::ok);
  CHECK(calls == 2);
}

TEST_CASE(
    "nested yield has a depth bound and does not double-count descendant "
    "time") {
  test::Fake platform;
  test::TestModule m;
  auto scheduler =
      core::make_scheduler<test::Event>(platform, core::ModuleList{&m}, 2);
  m.first_action = [&] {
    platform.advance(1);
    CHECK(scheduler.yield() == core::Status::ok);
    CHECK(scheduler.yield() == core::Status::ok);
    scheduler.stop();
  };
  m.second_action = [&] {
    platform.advance(2);
    CHECK(scheduler.yield() == core::Status::depth_limit);
  };
  m.third_action = [&] { platform.advance(3); };
  scheduler.schedule(m, &test::TestModule::first, 0);
  scheduler.schedule(m, &test::TestModule::second, 0);
  scheduler.schedule(m, &test::TestModule::third, 0);
  CHECK(scheduler.run() == core::Status::ok);
  CHECK(scheduler.snapshot().yield_depth_errors == 1);
  CHECK(scheduler.snapshot().tasks[0].total_nested_duration == 5);
  CHECK(scheduler.snapshot().tasks[0].total_self_duration == 1);
}

TEST_CASE(
    "yield rejects non-task call chains including foreign threads and "
    "interrupts") {
  test::Fake platform;
  test::TestModule m;
  auto scheduler =
      core::make_scheduler<test::Event>(platform, core::ModuleList{&m});
  CHECK(scheduler.yield() == core::Status::invalid_context);
  m.initializer = [&](core::InitStage) {
    CHECK(scheduler.yield() == core::Status::invalid_context);
    return core::Status::ok;
  };
  m.first_action = [&] {
    std::thread outsider(
        [&] { CHECK(scheduler.yield() == core::Status::invalid_context); });
    outsider.join();
    auto interrupt = [&] {
      CHECK(scheduler.yield() == core::Status::invalid_context);
    };
    platform.interrupt(
        [](void* ptr) { (*static_cast<decltype(interrupt)*>(ptr))(); },
        &interrupt);
    CHECK(scheduler.post(test::First{}) == core::Status::ok);
    CHECK(scheduler.yield() ==
          core::Status::empty);  // Event deliberately excluded.
  };
  m.receiver = [&](const test::Event&) {
    CHECK(scheduler.yield() == core::Status::invalid_context);
    scheduler.stop();
  };
  scheduler.schedule(m, &test::TestModule::first, 0);
  CHECK(scheduler.run() == core::Status::ok);
  CHECK(scheduler.yield() == core::Status::invalid_context);
  CHECK(scheduler.snapshot().invalid_yields == 7);
}

TEST_CASE(
    "stop in a nested task unwinds while preserving outer task lifetime") {
  test::Fake platform;
  test::TestModule m;
  auto scheduler =
      core::make_scheduler<test::Event>(platform, core::ModuleList{&m});
  bool returned = false;
  m.first_action = [&] {
    CHECK(scheduler.yield() == core::Status::ok);
    CHECK(scheduler.yield() == core::Status::not_running);
    returned = true;
  };
  m.second_action = [&] { scheduler.stop(); };
  scheduler.schedule(m, &test::TestModule::first, 0);
  scheduler.schedule(m, &test::TestModule::second, 0);
  CHECK(scheduler.run() == core::Status::ok);
  CHECK(returned);
  CHECK(scheduler.snapshot().tasks[0].executions == 1);
}

TEST_CASE(
    "command scopes exclude yielding even when called from a polling task") {
  test::Fake platform;
  test::TestModule m;

  // Match the test module's event type while retaining the command handler.
  struct CommandModule : core::Module<CommandModule, test::Event> {
    static constexpr const char* name() { return "commands"; }

    core::Status Try() { return scheduler().yield(); }

    static constexpr auto commands() {
      return std::array{DAVEOS_COMMAND(CommandModule, Try, "try", "test")};
    }
  } command;

  const auto modules = core::ModuleList{&m, &command};
  auto scheduler = core::make_scheduler<test::Event>(platform, modules);
  core::CommandDispatcher dispatcher(modules, scheduler);
  m.first_action = [&] {
    CHECK(dispatcher.dispatch("commands try") == core::Status::invalid_context);
    CHECK(scheduler.yield() == core::Status::empty);
    scheduler.stop();
  };
  scheduler.schedule(m, &test::TestModule::first, 0);
  CHECK(scheduler.run() == core::Status::ok);
  CHECK(scheduler.snapshot().invalid_yields == 1);
}

TEST_CASE("Application forwards configured yield depth") {
  test::Fake platform;
  test::TestModule m;
  auto app =
      core::make_application<test::Event, core::Capacities{.yield_depth = 1}>(
          platform, core::ModuleList{&m});
  auto& scheduler = app.scheduler();
  m.first_action = [&] {
    CHECK(scheduler.yield() == core::Status::depth_limit);
    scheduler.stop();
  };
  scheduler.schedule(m, &test::TestModule::first, 0);
  scheduler.schedule(m, &test::TestModule::second, 0);
  CHECK(app.run() == core::Status::ok);
}

TEST_CASE("nested callback time counts once and log attribution restores") {
  test::Fake platform;
  test::TestModule m;
  test::Sink sink;
  auto logger =
      core::make_logger(platform, core::SubscriberList{sink.subscriber()});
  auto scheduler =
      core::make_scheduler<test::Event>(platform, core::ModuleList{&m}, logger);
  m.first_action = [&] {
    scheduler.log(core::Level::info, "before");
    platform.advance(1);
    CHECK(scheduler.yield() == core::Status::ok);
    scheduler.log(core::Level::info, "after");
    scheduler.stop();
  };
  m.second_action = [&] {
    platform.advance(2);
    CHECK(scheduler.yield() == core::Status::ok);
    scheduler.log(core::Level::info, "middle");
  };
  m.third_action = [&] {
    platform.advance(3);
    scheduler.log(core::Level::info, "inner");
  };
  scheduler.schedule(m, &test::TestModule::first, 0);
  scheduler.schedule(m, &test::TestModule::second, 0);
  scheduler.schedule(m, &test::TestModule::third, 0);
  CHECK(scheduler.run() == core::Status::ok);
  const auto stats = scheduler.snapshot();
  CHECK(stats.tasks[0].total_duration == 6);
  CHECK(stats.tasks[0].total_nested_duration == 5);
  CHECK(stats.tasks[1].total_duration == 5);
  CHECK(stats.tasks[1].total_self_duration == 2);
#if DAVEOS_LOGGING
  REQUIRE(sink.records.size() == 4);
  CHECK(sink.records[0].task == "first");
  CHECK(sink.records[1].task == "third");
  CHECK(sink.records[2].task == "second");
  CHECK(sink.records[3].task == "first");
#endif
}

TEST_CASE(
    "overdue repeat iterations remain pending while their task is suspended") {
  test::Fake platform;
  test::TestModule m;
  auto scheduler =
      core::make_scheduler<test::Event>(platform, core::ModuleList{&m});
  std::uint32_t calls = 0;
  m.first_action = [&] {
    ++calls;
    if (calls == 1) {
      platform.advance(35);
      CHECK(scheduler.yield() == core::Status::ok);
      CHECK(calls == 1);
    } else if (calls == 4) {
      scheduler.stop();
    }
  };
  m.second_action = [&] { CHECK(scheduler.yield() == core::Status::empty); };
  scheduler.schedule(m, &test::TestModule::first, 10, core::Mode::repeat);
  scheduler.schedule(m, &test::TestModule::second, 11);
  CHECK(scheduler.run() == core::Status::ok);
  CHECK(calls == 4);
  CHECK(scheduler.snapshot().tasks[0].executions == 4);
}

TEST_CASE("logging and idle callbacks cannot yield") {
  test::Fake platform;

  struct Module : core::Module<Module> {
    std::function<bool()> sleeper;

    static constexpr const char* name() { return "idle"; }

    bool can_sleep() { return sleeper(); }
  } idle;

  std::function<void()> output;
  core::Subscriber subscriber{&output, [](void* p, const core::LogRecord&) {
                                (*static_cast<std::function<void()>*>(p))();
                              }};
  auto logger = core::make_logger(platform, core::SubscriberList{subscriber});
  auto scheduler =
      core::make_scheduler(platform, core::ModuleList{&idle}, logger);
  output = [&] { CHECK(scheduler.yield() == core::Status::invalid_context); };
  idle.sleeper = [&] {
    CHECK(scheduler.yield() == core::Status::invalid_context);
    scheduler.stop();
    return false;
  };
  scheduler.log(core::Level::info, "test");
  CHECK(scheduler.run() == core::Status::ok);
  CHECK(scheduler.snapshot().invalid_yields == (DAVEOS_LOGGING ? 2 : 1));
}

TEST_CASE(
    "a task cannot yield a different scheduler sharing thread-local context") {
  test::Fake first_platform, second_platform;
  test::TestModule a, b;
  auto first =
      core::make_scheduler<test::Event>(first_platform, core::ModuleList{&a});
  auto second =
      core::make_scheduler<test::Event>(second_platform, core::ModuleList{&b});
  a.first_action = [&] {
    CHECK(second.yield() == core::Status::invalid_context);
    CHECK(first.yield() == core::Status::empty);
    first.stop();
  };
  first.schedule(a, &test::TestModule::first, 0);
  CHECK(first.run() == core::Status::ok);
  CHECK(second.snapshot().invalid_yields == 1);
}
