#include "core/command/command.hpp"
#include "support.hpp"

namespace core = daveos::core;
namespace test = testing;

namespace {
  template <test::TestName Name>
  struct Motor : core::Module<Motor<Name>, test::Event> {
    static constexpr const char* name() { return Name.value; }

    static constexpr auto commands() {
      return std::array{
          DAVEOS_COMMAND(Motor, SetSpeed, "speed", "Set motor speed"),
          DAVEOS_COMMAND(Motor, Special, "special", "Special operation"),
          DAVEOS_COMMAND(Motor, Set, "set", "Set value"),
          DAVEOS_COMMAND(Motor, Settings, "settings", "Show settings")};
    }

    std::function<core::Status(core::CommandArguments)> action;

    core::Status SetSpeed(core::CommandArguments args) {
      I_("speed invoked");
      return action ? action(args) : core::Status::ok;
    }

    core::Status Special(core::CommandArguments) { return core::Status::full; }

    core::Status Set(core::CommandArguments) { return core::Status::empty; }

    core::Status Settings(core::CommandArguments) {
      return core::Status::truncated;
    }
  };

  struct Fixture {
    test::Fake platform;
    test::TestModule input;
    Motor<"motor"> motor;
    Motor<"motorboat"> boat;
    test::Sink sink;
    using List =
        core::ModuleList<test::TestModule, decltype(motor), decltype(boat)>;
    List modules{&input, &motor, &boat};
    decltype(core::make_logger(platform,
                               core::SubscriberList{sink.subscriber()})) logger{
        platform, core::SubscriberList{sink.subscriber()}};
    decltype(core::make_scheduler<test::Event>(platform, modules,
                                               logger)) scheduler =
        core::make_scheduler<test::Event>(platform, modules, logger);
    core::CommandDispatcher<test::Event, List> dispatcher{modules, scheduler};

    void Run(std::function<void()> action) {
      input.first_action = [&] {
        action();
        scheduler.stop();
      };
      REQUIRE(scheduler.schedule(input, &test::TestModule::first, 0) ==
              core::Status::ok);
      REQUIRE(scheduler.run() == core::Status::ok);
    }
  };
}  // namespace

TEST_CASE("commands tokenize once and preserve argument spelling") {
  Fixture f;
  std::vector<std::string> received;
  f.motor.action = [&](core::CommandArguments args) {
    for (auto arg : args) {
      received.emplace_back(arg);
    }
    return core::Status::ok;
  };
  f.Run([&] {
    CHECK(f.dispatcher.dispatch(
              R"cmd(MOTOR SPEED "" "a b" c\"d "e\"f" a\\b c\nd)cmd") ==
          core::Status::ok);
    CHECK(f.dispatcher.dispatch(R"cmd(motor speed "z\\")cmd") ==
          core::Status::ok);
  });
  CHECK(received == std::vector<std::string>{"", "a b", "c\"d", "e\"f", "a\\b",
                                             "c\\nd", "z\\"});
}

TEST_CASE("malformed or overflowing lines never call handlers") {
  Fixture f;
  int calls = 0;
  f.motor.action = [&](core::CommandArguments) {
    ++calls;
    return core::Status::ok;
  };
  f.Run([&] {
    for (auto text : {"motor speed \"unfinished", "motor speed ab\"cd\"",
                      "motor speed \"ab\"cd", "motor speed \"a\"\"b\""}) {
      CHECK(f.dispatcher.dispatch(text) == core::Status::parse_error);
    }
    CHECK(f.dispatcher.dispatch(std::string_view("motor speed a\0b", 15)) ==
          core::Status::parse_error);
    CHECK(f.dispatcher.dispatch(std::string(257, ' ')) ==
          core::Status::line_too_long);
    std::string text = "motor speed";
    for (int i = 0; i < 7; ++i) {
      text += " x";
    }
    CHECK(f.dispatcher.dispatch(text) == core::Status::too_many_arguments);
    CHECK(f.dispatcher.dispatch(" \t\r\n\v\f") == core::Status::ok);
  });
  CHECK(calls == 0);
}

TEST_CASE("input and argument capacities include exactly their stated limits") {
  Fixture f;
  std::size_t count = 0, length = 0;
  f.motor.action = [&](core::CommandArguments args) {
    count = args.size();
    length = args[0].size();
    return core::Status::ok;
  };
  f.Run([&] {
    CHECK(f.dispatcher.dispatch("motor speed " + std::string(244, 'X')) ==
          core::Status::ok);
    CHECK(length == 244);
    std::string text = "motor speed";
    for (int i = 0; i < 6; ++i) {
      text += " x";
    }
    CHECK(f.dispatcher.dispatch(text) == core::Status::ok);
    CHECK(count == 6);
    core::CommandDispatcher<test::Event, Fixture::List, 17, 3> small(
        f.modules, f.scheduler);
    CHECK(small.dispatch("motor speed 12345") == core::Status::ok);
    CHECK(length == 5);
    CHECK(small.dispatch("motor speed 123456") == core::Status::line_too_long);
    CHECK(small.dispatch("motor speed x y") ==
          core::Status::too_many_arguments);
  });
}

TEST_CASE("routing prefers exact names and diagnoses unique-prefix failures") {
  Fixture f;
  f.Run([&] {
    CHECK(f.dispatcher.dispatch("motor spee") == core::Status::ok);
    CHECK(f.dispatcher.dispatch("MOTORB SPEED") == core::Status::ok);
    CHECK(f.dispatcher.dispatch("mot speed") == core::Status::ambiguous_match);
    CHECK(f.dispatcher.dispatch("motor sp") == core::Status::ambiguous_match);
    CHECK(f.dispatcher.dispatch("motor set") == core::Status::empty);
    CHECK(f.dispatcher.dispatch("motor sett") == core::Status::truncated);
    CHECK(f.dispatcher.dispatch("motor special") == core::Status::full);
    CHECK(f.dispatcher.dispatch("absent speed") == core::Status::not_found);
    CHECK(f.dispatcher.dispatch("motor absent") == core::Status::not_found);
    CHECK(f.dispatcher.dispatch("input") == core::Status::not_found);
    CHECK(f.dispatcher.dispatch("\"\" speed") == core::Status::not_found);
  });
}

TEST_CASE("help dumps the tree and supports only the agreed forms") {
  Fixture f;
  f.Run([&] {
    CHECK(f.dispatcher.dispatch("help") == core::Status::ok);
    CHECK(f.dispatcher.dispatch("motor") == core::Status::ok);
    CHECK(f.dispatcher.dispatch("motor h") == core::Status::ok);
    CHECK(f.dispatcher.dispatch("help motor") ==
          core::Status::invalid_argument);
    CHECK(f.dispatcher.dispatch("motor help extra") ==
          core::Status::invalid_argument);
  });
#if DAVEOS_LOGGING
  int headings = 0, descriptions = 0;
  for (const auto& record : f.sink.records) {
    CHECK(record.module == "core");
    CHECK(record.task == "command");
    if (record.message == "motor:") {
      ++headings;
    }
    if (record.message.find("Set motor speed") != std::string::npos) {
      ++descriptions;
    }
    CHECK(record.message != "module:");
  }
  CHECK(headings == 3);
  CHECK(descriptions == 4);
#else
  CHECK(f.sink.records.empty());
#endif
}

TEST_CASE(
    "handler context and argument views survive nested dispatch and "
    "interrupts") {
  Fixture f;
  f.motor.action = [&](core::CommandArguments args) {
    REQUIRE(args.size() == 1);
    CHECK(args[0] == "Original");
    CHECK(f.dispatcher.dispatch("motor speed overwritten") ==
          core::Status::busy);
    CHECK(args[0] == "Original");
    CHECK(std::string_view(f.platform.context().module) == "motor");
    CHECK(std::string_view(f.platform.context().task) == "SetSpeed");
    f.platform.interrupt(
        [](void* pointer) {
          auto& fixture = *static_cast<Fixture*>(pointer);
          CHECK(fixture.dispatcher.dispatch("help") ==
                core::Status::invalid_argument);
          fixture.scheduler.log(core::Level::info, "interrupt log");
        },
        &f);
    return core::Status::invalid_argument;
  };
  CHECK(f.dispatcher.dispatch("help") == core::Status::not_running);
  CHECK(f.scheduler.init() == core::Status::ok);
  CHECK(f.dispatcher.dispatch("help") == core::Status::not_running);
  f.Run([&] {
    CHECK(f.dispatcher.dispatch("motor speed Original") ==
          core::Status::invalid_argument);
    f.scheduler.log(core::Level::info, "restored");
  });
#if DAVEOS_LOGGING
  REQUIRE(f.sink.records.size() == 5);
  CHECK(f.sink.records[0].module == "motor");
  CHECK(f.sink.records[0].task == "SetSpeed");
  CHECK(f.sink.records[1].module == "core");
  CHECK(f.sink.records[1].task == "command");
  CHECK(f.sink.records[2].task == "interrupt");
  CHECK(f.sink.records[3].task == "command");
  CHECK(f.sink.records[4].module == "module");
  CHECK(f.sink.records[4].task == "first");
#else
  CHECK(f.sink.records.empty());
#endif
  CHECK(f.dispatcher.dispatch("help") == core::Status::not_running);
}

TEST_CASE(
    "a scheduler with no task slots can initialize and run commands from "
    "events") {
  struct OnlyCommands : core::Module<OnlyCommands, test::Event> {
    static constexpr const char* name() { return "display name"; }

    static constexpr const char* command_prefix() { return "console"; }

    static constexpr auto commands() {
      return std::array{DAVEOS_COMMAND(OnlyCommands, Exit, "exit", "Stop")};
    }

    std::function<void()> action;

    void on_event(const test::Event&) { action(); }

    core::Status Exit(core::CommandArguments args) {
      return args.empty() ? scheduler().stop() : core::Status::invalid_argument;
    }
  } module;

  test::Fake platform;
  auto modules = core::ModuleList{&module};
  auto scheduler = core::make_scheduler<test::Event>(platform, modules);
  core::CommandDispatcher dispatcher(modules, scheduler);
  module.action = [&] {
    CHECK(dispatcher.dispatch("console exit") == core::Status::ok);
  };
  CHECK(scheduler.post(test::First{}) == core::Status::ok);
  CHECK(scheduler.run() == core::Status::ok);
  CHECK(scheduler.snapshot().tasks.empty());
}

TEST_CASE("logging overflow does not replace command results") {
  Fixture f;
  f.Run([&] {
    for (int i = 0; i < 40; ++i) {
      CHECK(f.dispatcher.dispatch("help") == core::Status::ok);
    }
    CHECK(f.dispatcher.dispatch("motor special") == core::Status::full);
  });
  CHECK((f.logger.counters().dropped > 0) == bool(DAVEOS_LOGGING));
}

TEST_CASE("Independent console sources share parsing and command dispatch") {
  Fixture f;
  core::CommandSource uart, usb;
  CHECK(uart.dispatch("motor speed 0") == core::Status::not_running);
  core::CommandDispatcher dispatcher(f.modules, f.scheduler);
  CHECK(uart.dispatch("help") == core::Status::not_running);
  dispatcher.bind_sources(core::CommandSourceList{uart, usb});
  std::vector<std::string> received;
  f.motor.action = [&](core::CommandArguments args) {
    for (auto arg : args) {
      received.emplace_back(arg);
    }
    return core::Status::ok;
  };
  f.Run([&] {
    CHECK(uart.dispatch("motor speed 10") == core::Status::ok);
    CHECK(usb.dispatch(R"(motor speed "two words")") == core::Status::ok);
    CHECK(usb.dispatch("motor speed \"bad") == core::Status::parse_error);
    CHECK(uart.dispatch("motor speed 30") == core::Status::ok);
  });
  CHECK(received == std::vector<std::string>{"10", "two words", "30"});
}

TEST_CASE("A dispatcher can register no command sources") {
  Fixture f;
  core::CommandDispatcher dispatcher(f.modules, f.scheduler);
  dispatcher.bind_sources(core::CommandSourceList{});
  f.Run(
      [&] { CHECK(dispatcher.dispatch("motor speed 1") == core::Status::ok); });
}

TEST_CASE("raw handlers can explicitly raise the default six-argument limit") {
  Fixture f;
  std::size_t count = 0;
  f.motor.action = [&](core::CommandArguments args) {
    count = args.size();
    return core::Status::ok;
  };
  f.Run([&] {
    constexpr auto line = "motor speed 1 2 3 4 5 6 7 8 9 10";
    CHECK(f.dispatcher.dispatch(line) == core::Status::too_many_arguments);
    CHECK(count == 0);
    core::CommandDispatcher<test::Event, Fixture::List, 256, 12> larger(
        f.modules, f.scheduler);
    CHECK(larger.dispatch(line) == core::Status::ok);
    CHECK(count == 10);
  });
}
