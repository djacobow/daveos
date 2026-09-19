#include "core/command/command.hpp"
#include "support.hpp"

namespace core = daveos::core;
namespace test = testing;

namespace {
#define TEST_CHOICE_MODES(X) X(on, -7) X(once, 10) X(off, 50) X(toggle, 100)
  DAVEOS_ENUM(Mode, std::int16_t, TEST_CHOICE_MODES)
#undef TEST_CHOICE_MODES

  inline constexpr std::array labels{
      core::EnumChoice{"always on", Mode::on},
      core::EnumChoice{"one shot", Mode::once},
      core::EnumChoice{"disabled", Mode::off},
      core::EnumChoice{"toggle", Mode::toggle},
      core::EnumChoice{"flip", Mode::toggle},
      core::EnumChoice{"flip once", Mode::toggle}};

  struct Choices : core::Module<Choices, test::Event> {
    static constexpr const char* name() { return "choices"; }

    std::size_t calls = 0;
    Mode selected = Mode::off;
    std::optional<Mode> fallback;

    core::Status Apply(Mode value, std::optional<Mode> other) {
      ++calls;
      selected = value;
      fallback = other;
      I_("selected %s", enum_name(value));
      return core::Status::ok;
    }

    core::Status Optional(std::optional<Mode> value) {
      ++calls;
      fallback = value;
      return core::Status::ok;
    }

    static constexpr auto commands() {
      return std::array{
          DAVEOS_COMMAND(Choices, Apply, "apply", "Select modes",
                         core::arg("mode"), core::arg("fallback")),
          DAVEOS_COMMAND(Choices, Apply, "labels", "Custom labels",
                         core::arg("mode").choices<labels>(),
                         core::arg("fallback").choices<labels>()),
          DAVEOS_COMMAND(Choices, Optional, "optional", "Optional mode",
                         core::arg("mode"))};
    }
  };

  template <typename Action>
  auto Run(Action action) {
    test::Fake platform;
    Choices module;
    test::TestModule runner;
    test::Sink sink;
    auto modules = core::ModuleList{&module, &runner};
    auto logger =
        core::make_logger(platform, core::SubscriberList{sink.subscriber()});
    auto scheduler =
        core::make_scheduler<test::Event>(platform, modules, logger);
    core::CommandDispatcher dispatcher(modules, scheduler);
    runner.first_action = [&] {
      action(module, dispatcher);
      scheduler.stop();
    };
    REQUIRE(scheduler.schedule(runner, &test::TestModule::first, 0) ==
            core::Status::ok);
    REQUIRE(scheduler.run() == core::Status::ok);
    return sink.records;
  }
}  // namespace

TEST_CASE("enum helper exposes names and actual sparse signed values") {
  constexpr auto entries = enum_choices(Mode{});
  STATIC_REQUIRE(entries.size() == 4);
  STATIC_REQUIRE(std::string_view(entries[0].name) == "on");
  STATIC_REQUIRE(entries[0].value == Mode::on);
  STATIC_REQUIRE(static_cast<std::int16_t>(entries[0].value) == -7);
  STATIC_REQUIRE(entries[2].value == Mode::off);
  STATIC_REQUIRE(std::string_view(enum_name(Mode::toggle)) == "toggle");
}

TEST_CASE(
    "required and optional enum choices share exact and unique-prefix "
    "matching") {
  Run([](auto& module, auto& dispatcher) {
    constexpr std::array<std::pair<std::string_view, Mode>, 12> accepted{
        {{"on", Mode::on},
         {"ON", Mode::on},
         {"On", Mode::on},
         {"once", Mode::once},
         {"onc", Mode::once},
         {"OnC", Mode::once},
         {"off", Mode::off},
         {"of", Mode::off},
         {"OF", Mode::off},
         {"toggle", Mode::toggle},
         {"t", Mode::toggle},
         {"TOG", Mode::toggle}}};
    for (const auto& [text, value] : accepted) {
      CAPTURE(text);
      CHECK(dispatcher.dispatch(std::string("choices apply ") +
                                std::string(text)) == core::Status::ok);
      CHECK(module.selected == value);
      CHECK_FALSE(module.fallback.has_value());
      CHECK(dispatcher.dispatch(std::string("choices apply off ") +
                                std::string(text)) == core::Status::ok);
      CHECK(module.fallback == value);
      CHECK(dispatcher.dispatch(std::string("choices optional ") +
                                std::string(text)) == core::Status::ok);
      CHECK(module.fallback == value);
    }
    const auto calls = module.calls;
    for (auto prefix :
         {"choices apply ", "choices apply on ", "choices optional "}) {
      for (auto text : {"o", "O"}) {
        CHECK(dispatcher.dispatch(std::string(prefix) + text) ==
              core::Status::ambiguous_match);
        CHECK(module.calls == calls);
      }
      for (auto text :
           {"unknown", "onward", "-7", "10", "\"\"", "\"on \"", "\" on\""}) {
        CHECK(dispatcher.dispatch(std::string(prefix) + text) ==
              core::Status::not_found);
        CHECK(module.calls == calls);
      }
    }
    CHECK(dispatcher.dispatch("choices apply") ==
          core::Status::invalid_argument);
    CHECK(dispatcher.dispatch("choices apply on off toggle") ==
          core::Status::invalid_argument);
    CHECK(module.calls == calls);
    CHECK(dispatcher.dispatch("choices optional") == core::Status::ok);
    CHECK_FALSE(module.fallback.has_value());
  });
}

TEST_CASE(
    "custom choice labels permit aliases and quoted spaces without ordinal "
    "assumptions") {
  Run([](auto& module, auto& dispatcher) {
    CHECK(dispatcher.dispatch(R"(choices labels "ALWAYS O" "one s")") ==
          core::Status::ok);
    CHECK(module.selected == Mode::on);
    CHECK(module.fallback == Mode::once);
    CHECK(dispatcher.dispatch("choices labels flip dis") == core::Status::ok);
    CHECK(module.selected == Mode::toggle);
    CHECK(module.fallback == Mode::off);
    CHECK(dispatcher.dispatch("choices labels tog") == core::Status::ok);
    CHECK(module.selected == Mode::toggle);
    CHECK_FALSE(module.fallback.has_value());
    // An explicit table replaces the inferred table; identifier 'off' is
    // absent.
    const auto calls = module.calls;
    CHECK(dispatcher.dispatch("choices labels off") == core::Status::not_found);
    CHECK(dispatcher.dispatch("choices labels fl") ==
          core::Status::ambiguous_match);
    CHECK(module.calls == calls);
  });
}

TEST_CASE(
    "plain enums with full-width values work with explicit choice tables") {
  enum class Wide : std::uint64_t { big = UINT64_MAX, small = 7 };
  static constexpr std::array table{core::EnumChoice{"big", Wide::big},
                                    core::EnumChoice{"small", Wide::small}};

  struct Receiver {
    Wide received{};

    core::Status Apply(Wide value) {
      received = value;
      return core::Status::ok;
    }
  } module;

  constexpr auto descriptor = core::command<&Receiver::Apply>(
      "apply", "Choose", core::arg("value").choices<table>());
  const std::array<std::string_view, 1> args{"B"};
  core::ArgumentError error;
  CHECK(descriptor.callback(module, args, descriptor, error) ==
        core::Status::ok);
  CHECK(module.received == Wide::big);
  CHECK(error.reason == nullptr);
}

TEST_CASE(
    "choice help and errors identify the parameter and allowed spellings") {
  auto records = Run([](auto&, auto& dispatcher) {
    CHECK(dispatcher.dispatch("choices apply o") ==
          core::Status::ambiguous_match);
    CHECK(dispatcher.dispatch("choices apply on unknown") ==
          core::Status::not_found);
    CHECK(dispatcher.dispatch("choices apply once") == core::Status::ok);
    CHECK(dispatcher.dispatch("choices help") == core::Status::ok);
  });
#if DAVEOS_LOGGING
  bool ambiguous = false, unknown = false, choice = false, handler = false;
  for (const auto& record : records) {
    ambiguous |= record.message.find("argument 'mode': ambiguous choice") !=
                 std::string::npos;
    unknown |= record.message.find("argument 'fallback': unknown choice") !=
               std::string::npos;
    choice |= record.message == "      once";
    handler |= record.module == "choices" && record.task == "Apply" &&
               record.message == "selected once";
  }
  CHECK(ambiguous);
  CHECK(unknown);
  CHECK(choice);
  CHECK(handler);
#else
  CHECK(records.empty());
#endif
}

TEST_CASE(
    "bad choices produce one specific diagnostic without a generic duplicate") {
  auto records = Run([](auto&, auto& dispatcher) {
    CHECK(dispatcher.dispatch("choices apply o") ==
          core::Status::ambiguous_match);
    CHECK(dispatcher.dispatch("choices apply on unknown") ==
          core::Status::not_found);
    CHECK(dispatcher.dispatch("unknown apply") == core::Status::not_found);
  });
#if DAVEOS_LOGGING
  REQUIRE(records.size() == 3);
  CHECK(records[0].message.find("argument 'mode': ambiguous choice") !=
        std::string::npos);
  CHECK(records[1].message.find("argument 'fallback': unknown choice") !=
        std::string::npos);
  CHECK(records[2].message.find("unknown command") != std::string::npos);
#endif
}
