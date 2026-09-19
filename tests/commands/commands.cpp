#include "core/command/command.hpp"
#include "support.hpp"
using namespace testing;
namespace {
template <TestName Name>
struct Motor : Module<Motor<Name>, Event> {
  static constexpr const char* name() { return Name.value; }
  static constexpr auto commands() {
    return std::array{
        DAVEOS_COMMAND(Motor, "speed", SetSpeed, "Set motor speed"),
        DAVEOS_COMMAND(Motor, "special", Special, "Special operation"),
        DAVEOS_COMMAND(Motor, "set", Set, "Set value"),
        DAVEOS_COMMAND(Motor, "settings", Settings, "Show settings")};
  }
  std::function<Status(CommandArguments)> action;
  Status SetSpeed(CommandArguments args) {
    I_("speed invoked");
    return action ? action(args) : Status::ok;
  }
  Status Special(CommandArguments) { return Status::full; }
  Status Set(CommandArguments) { return Status::empty; }
  Status Settings(CommandArguments) { return Status::truncated; }
};
struct Fixture {
  Fake platform;
  TestModule input;
  Motor<"motor"> motor;
  Motor<"motorboat"> boat;
  Sink sink;
  using List = ModuleList<TestModule, decltype(motor), decltype(boat)>;
  List modules{&input, &motor, &boat};
  decltype(make_logger(platform, SubscriberList{sink.subscriber()})) logger{
      platform, SubscriberList{sink.subscriber()}};
  decltype(make_scheduler<Event>(platform, modules, logger)) scheduler =
      make_scheduler<Event>(platform, modules, logger);
  CommandDispatcher<Event, List> dispatcher{modules, scheduler};
  void Run(std::function<void()> action) {
    input.first_action = [&] {
      action();
      scheduler.stop();
    };
    REQUIRE(scheduler.schedule(input, &TestModule::first, 0) == Status::ok);
    REQUIRE(scheduler.run() == Status::ok);
  }
};
}  // namespace

TEST_CASE("commands tokenize once and preserve argument spelling") {
  Fixture f;
  std::vector<std::string> received;
  f.motor.action = [&](CommandArguments args) {
    for (auto arg : args) received.emplace_back(arg);
    return Status::ok;
  };
  f.Run([&] {
    CHECK(f.dispatcher.dispatch(
              R"cmd(MOTOR SPEED "" "a b" c\"d "e\"f" a\\b c\nd "z\\")cmd") ==
          Status::ok);
  });
  CHECK(received == std::vector<std::string>{"", "a b", "c\"d", "e\"f", "a\\b",
                                             "c\\nd", "z\\"});
}

TEST_CASE("malformed or overflowing lines never call handlers") {
  Fixture f;
  int calls = 0;
  f.motor.action = [&](CommandArguments) {
    ++calls;
    return Status::ok;
  };
  f.Run([&] {
    for (auto text : {"motor speed \"unfinished", "motor speed ab\"cd\"",
                      "motor speed \"ab\"cd", "motor speed \"a\"\"b\""})
      CHECK(f.dispatcher.dispatch(text) == Status::parse_error);
    CHECK(f.dispatcher.dispatch(std::string_view("motor speed a\0b", 15)) ==
          Status::parse_error);
    CHECK(f.dispatcher.dispatch(std::string(257, ' ')) ==
          Status::line_too_long);
    std::string text = "motor speed";
    for (int i = 0; i < 15; ++i) text += " x";
    CHECK(f.dispatcher.dispatch(text) == Status::too_many_arguments);
    CHECK(f.dispatcher.dispatch(" \t\r\n\v\f") == Status::ok);
  });
  CHECK(calls == 0);
}

TEST_CASE("input and argument capacities include exactly their stated limits") {
  Fixture f;
  std::size_t count = 0, length = 0;
  f.motor.action = [&](CommandArguments args) {
    count = args.size();
    length = args[0].size();
    return Status::ok;
  };
  f.Run([&] {
    CHECK(f.dispatcher.dispatch("motor speed " + std::string(244, 'X')) ==
          Status::ok);
    CHECK(length == 244);
    std::string text = "motor speed";
    for (int i = 0; i < 14; ++i) text += " x";
    CHECK(f.dispatcher.dispatch(text) == Status::ok);
    CHECK(count == 14);
    CommandDispatcher<Event, Fixture::List, 17, 3> small(f.modules,
                                                         f.scheduler);
    CHECK(small.dispatch("motor speed 12345") == Status::ok);
    CHECK(length == 5);
    CHECK(small.dispatch("motor speed 123456") == Status::line_too_long);
    CHECK(small.dispatch("motor speed x y") == Status::too_many_arguments);
  });
}

TEST_CASE("routing prefers exact names and diagnoses unique-prefix failures") {
  Fixture f;
  f.Run([&] {
    CHECK(f.dispatcher.dispatch("motor spee") == Status::ok);
    CHECK(f.dispatcher.dispatch("MOTORB SPEED") == Status::ok);
    CHECK(f.dispatcher.dispatch("mot speed") == Status::ambiguous_match);
    CHECK(f.dispatcher.dispatch("motor sp") == Status::ambiguous_match);
    CHECK(f.dispatcher.dispatch("motor set") == Status::empty);
    CHECK(f.dispatcher.dispatch("motor sett") == Status::truncated);
    CHECK(f.dispatcher.dispatch("motor special") == Status::full);
    CHECK(f.dispatcher.dispatch("absent speed") == Status::not_found);
    CHECK(f.dispatcher.dispatch("motor absent") == Status::not_found);
    CHECK(f.dispatcher.dispatch("input") == Status::not_found);
    CHECK(f.dispatcher.dispatch("\"\" speed") == Status::not_found);
  });
}

TEST_CASE("help dumps the tree and supports only the agreed forms") {
  Fixture f;
  f.Run([&] {
    CHECK(f.dispatcher.dispatch("help") == Status::ok);
    CHECK(f.dispatcher.dispatch("motor") == Status::ok);
    CHECK(f.dispatcher.dispatch("motor h") == Status::ok);
    CHECK(f.dispatcher.dispatch("help motor") == Status::invalid_argument);
    CHECK(f.dispatcher.dispatch("motor help extra") ==
          Status::invalid_argument);
  });
#if DAVEOS_LOGGING
  int headings = 0, descriptions = 0;
  for (const auto& record : f.sink.records) {
    CHECK(record.module == "core");
    CHECK(record.task == "command");
    if (record.message == "motor:") ++headings;
    if (record.message.find("Set motor speed") != std::string::npos)
      ++descriptions;
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
  f.motor.action = [&](CommandArguments args) {
    REQUIRE(args.size() == 1);
    CHECK(args[0] == "Original");
    CHECK(f.dispatcher.dispatch("motor speed overwritten") == Status::busy);
    CHECK(args[0] == "Original");
    CHECK(std::string_view(f.platform.context().module) == "motor");
    CHECK(std::string_view(f.platform.context().task) == "SetSpeed");
    f.platform.interrupt(
        [](void* pointer) {
          auto& fixture = *static_cast<Fixture*>(pointer);
          CHECK(fixture.dispatcher.dispatch("help") ==
                Status::invalid_argument);
          fixture.scheduler.log(Level::info, "interrupt log");
        },
        &f);
    return Status::invalid_argument;
  };
  CHECK(f.dispatcher.dispatch("help") == Status::not_running);
  CHECK(f.scheduler.init() == Status::ok);
  CHECK(f.dispatcher.dispatch("help") == Status::not_running);
  f.Run([&] {
    CHECK(f.dispatcher.dispatch("motor speed Original") ==
          Status::invalid_argument);
    f.scheduler.log(Level::info, "restored");
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
  CHECK(f.dispatcher.dispatch("help") == Status::not_running);
}

TEST_CASE(
    "a scheduler with no task slots can initialize and run commands from "
    "events") {
  struct OnlyCommands : Module<OnlyCommands, Event> {
    static constexpr const char* name() { return "display name"; }
    static constexpr const char* command_prefix() { return "console"; }
    static constexpr auto commands() {
      return std::array{DAVEOS_COMMAND(OnlyCommands, "exit", Exit, "Stop")};
    }
    std::function<void()> action;
    void on_event(Event) { action(); }
    Status Exit(CommandArguments args) {
      return args.empty() ? scheduler().stop() : Status::invalid_argument;
    }
  } module;
  Fake platform;
  auto modules = ModuleList{&module};
  auto scheduler = make_scheduler<Event>(platform, modules);
  CommandDispatcher dispatcher(modules, scheduler);
  module.action = [&] {
    CHECK(dispatcher.dispatch("console exit") == Status::ok);
  };
  CHECK(scheduler.post(Event::first) == Status::ok);
  CHECK(scheduler.run() == Status::ok);
  CHECK(scheduler.snapshot().tasks.empty());
}

TEST_CASE("logging overflow does not replace command results") {
  Fixture f;
  f.Run([&] {
    for (int i = 0; i < 40; ++i)
      CHECK(f.dispatcher.dispatch("help") == Status::ok);
    CHECK(f.dispatcher.dispatch("motor special") == Status::full);
  });
  CHECK((f.logger.counters().dropped > 0) == bool(DAVEOS_LOGGING));
}

TEST_CASE("Independent console sources share parsing and command dispatch") {
  Fixture f;
  CommandSource uart, usb;
  CHECK(uart.dispatch("motor speed 0") == Status::not_running);
  CommandDispatcher dispatcher(f.modules, f.scheduler);
  CHECK(uart.dispatch("help") == Status::not_running);
  dispatcher.bind_sources(CommandSourceList{uart, usb});
  std::vector<std::string> received;
  f.motor.action = [&](CommandArguments args) {
    for (auto arg : args) received.emplace_back(arg);
    return Status::ok;
  };
  f.Run([&] {
    CHECK(uart.dispatch("motor speed 10") == Status::ok);
    CHECK(usb.dispatch(R"(motor speed "two words")") == Status::ok);
    CHECK(usb.dispatch("motor speed \"bad") == Status::parse_error);
    CHECK(uart.dispatch("motor speed 30") == Status::ok);
  });
  CHECK(received == std::vector<std::string>{"10", "two words", "30"});
}

TEST_CASE("A dispatcher can register no command sources") {
  Fixture f;
  CommandDispatcher dispatcher(f.modules, f.scheduler);
  dispatcher.bind_sources(CommandSourceList{});
  f.Run([&] { CHECK(dispatcher.dispatch("motor speed 1") == Status::ok); });
}
