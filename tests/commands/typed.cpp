#include "core/command/command.hpp"
#include "support.hpp"

namespace core = daveos::core;
namespace test = testing;

namespace {
  struct Typed : core::Module<Typed, test::Event> {
    static constexpr const char* name() { return "typed"; }

    int calls = 0;
    std::int64_t integer = 0;
    std::uint64_t unsigned_integer = 0;
    double real = 0;
    bool boolean = false;
    std::optional<std::uint32_t> optional;
    std::string text;
    std::optional<std::string> tail;
    std::optional<bool> enabled;
    std::optional<double> ratio;

    core::Status Text(std::string_view value,
                      std::optional<std::string_view> suffix,
                      std::optional<bool> flag, std::optional<double> amount) {
      ++calls;
      text = value;
      tail = suffix ? std::optional<std::string>(*suffix) : std::nullopt;
      enabled = flag;
      ratio = amount;
      return core::Status::ok;
    }

    core::Status OptionalOnly(std::optional<std::uint32_t> count,
                              std::optional<bool> flag,
                              std::optional<std::string_view> suffix) {
      ++calls;
      optional = count;
      enabled = flag;
      tail = suffix ? std::optional<std::string>(*suffix) : std::nullopt;
      return core::Status::ok;
    }

    core::Status Signed(std::int64_t value) {
      ++calls;
      integer = value;
      return core::Status::ok;
    }

    core::Status Unsigned(std::uint64_t value) {
      ++calls;
      unsigned_integer = value;
      return core::Status::ok;
    }

    core::Status Small(std::int8_t value) {
      ++calls;
      integer = value;
      return core::Status::ok;
    }

    core::Status Float(double value) {
      ++calls;
      real = value;
      return core::Status::ok;
    }

    core::Status Boolean(bool value) {
      ++calls;
      boolean = value;
      return core::Status::ok;
    }

    core::Status Sample(float value, std::optional<std::uint32_t> count) {
      ++calls;
      real = value;
      optional = count;
      I_("sample invoked");
      return core::Status::ok;
    }

    core::Status Empty() const noexcept { return core::Status::full; }

    static constexpr auto commands() {
      return std::array{
          DAVEOS_COMMAND(Typed, Signed, "signed", "Signed integer",
                         core::arg("value")),
          DAVEOS_COMMAND(Typed, Unsigned, "unsigned", "Unsigned integer",
                         core::arg("value")),
          DAVEOS_COMMAND(Typed, Small, "small", "Small integer",
                         core::arg("value")),
          DAVEOS_COMMAND(Typed, Float, "float", "Floating point",
                         core::arg("value")),
          DAVEOS_COMMAND(Typed, Boolean, "strict", "Strict boolean",
                         core::arg("enabled")),
          DAVEOS_COMMAND(Typed, Boolean, "friendly", "Friendly boolean",
                         core::arg("enabled").friendly()),
          DAVEOS_COMMAND(Typed, Sample, "sample", "Collect samples",
                         core::arg("rate").range(0.5f, 1000.0f),
                         core::arg("count").range(1u, 100u)),
          DAVEOS_COMMAND(Typed, Empty, "empty", "No arguments"),
          DAVEOS_COMMAND(Typed, OptionalOnly, "optional", "All optional",
                         core::arg("count").range(1u, 3u),
                         core::arg("enabled").friendly(), core::arg("suffix")),
          DAVEOS_COMMAND(Typed, Text, "text", "Text and optional values",
                         core::arg("text"), core::arg("suffix"),
                         core::arg("enabled").friendly(),
                         core::arg("ratio").range(0.5, 2.0)),
          DAVEOS_COMMAND(Typed, Unsigned, "maximum", "Exact large bound",
                         core::arg("value").min(UINT64_MAX))};
    }
  };

  template <typename Action>
  auto Run(Action action) {
    test::Fake platform;
    Typed module;
    test::TestModule input;
    test::Sink sink;
    auto modules = core::ModuleList{&module, &input};
    auto logger =
        core::make_logger(platform, core::SubscriberList{sink.subscriber()});
    auto scheduler =
        core::make_scheduler<test::Event>(platform, modules, logger);
    core::CommandDispatcher<test::Event, decltype(modules)> dispatcher{
        modules, scheduler};
    input.first_action = [&] {
      action(module, dispatcher);
      scheduler.stop();
    };
    REQUIRE(scheduler.schedule(input, &test::TestModule::first, 0) ==
            core::Status::ok);
    REQUIRE(scheduler.run() == core::Status::ok);
    return sink.records;
  }
}  // namespace

TEST_CASE("typed integers preserve limits and consume whole tokens") {
  Run([](auto& m, auto& d) {
    CHECK(d.dispatch("typed signed -9223372036854775808") == core::Status::ok);
    CHECK(m.integer == INT64_MIN);
    CHECK(d.dispatch("typed signed 9223372036854775807") == core::Status::ok);
    CHECK(m.integer == INT64_MAX);
    CHECK(d.dispatch("typed unsigned 18446744073709551615") ==
          core::Status::ok);
    CHECK(m.unsigned_integer == UINT64_MAX);
    CHECK(d.dispatch("typed maximum 18446744073709551614") ==
          core::Status::invalid_argument);
    CHECK(d.dispatch("typed maximum 18446744073709551615") == core::Status::ok);
    for (auto text : {"typed small -128", "typed small 127",
                      "typed small -0x80", "typed small +0b1111111"}) {
      CHECK(d.dispatch(text) == core::Status::ok);
    }
    CHECK(d.dispatch("typed small 010") == core::Status::ok);
    CHECK(m.integer == 10);
    const auto calls = m.calls;
    for (auto text :
         {"typed signed -9223372036854775809",
          "typed signed 9223372036854775808",
          "typed unsigned 18446744073709551616", "typed unsigned -1",
          "typed small 128", "typed small -129", "typed small 12x",
          "typed small 1.0", "typed small 0x", "typed small 0b2",
          "typed small --1", "typed small +", "typed small \" 1\"",
          "typed small \"\"", "typed small", "typed small 1 2"}) {
      CHECK(d.dispatch(text) == core::Status::invalid_argument);
    }
    CHECK(m.calls == calls);
  });
}

TEST_CASE(
    "typed floats and trailing optional values validate before invocation") {
  Run([](auto& m, auto& d) {
    CHECK(d.dispatch("typed float -1.25e2") == core::Status::ok);
    CHECK(m.real == -125);
    CHECK(d.dispatch("typed sample 0.5") == core::Status::ok);
    CHECK_FALSE(m.optional.has_value());
    CHECK(d.dispatch("typed sample 1000 100") == core::Status::ok);
    CHECK(m.optional == 100);
    const auto calls = m.calls;
    for (auto text :
         {"typed float +-1", "typed float nan", "typed float inf",
          "typed float -inf", "typed float 1e9999", "typed float 1e-9999",
          "typed float 1.5junk", "typed float 0x1p2", "typed float \"\"",
          "typed sample 0.49", "typed sample 1001", "typed sample 1 0",
          "typed sample 1 101", "typed sample 1 junk", "typed sample",
          "typed sample 1 2 3"}) {
      CHECK(d.dispatch(text) == core::Status::invalid_argument);
    }
    CHECK(m.calls == calls);
    CHECK(d.dispatch("typed sample 25.5") == core::Status::ok);
    CHECK_FALSE(m.optional.has_value());
    CHECK(d.dispatch("typed empty") == core::Status::full);
    CHECK(d.dispatch("typed empty extra") == core::Status::invalid_argument);
  });
}

TEST_CASE("friendly booleans accept only documented aliases") {
  Run([](auto& m, auto& d) {
    for (auto text : {"true", "1", "ON", "yes", "Enable", "high", "SET"}) {
      CHECK(d.dispatch(std::string("typed friendly ") + text) ==
            core::Status::ok);
      CHECK(m.boolean);
    }
    for (auto text : {"false", "0", "OFF", "no", "Disable", "low", "CLEAR"}) {
      CHECK(d.dispatch(std::string("typed friendly ") + text) ==
            core::Status::ok);
      CHECK_FALSE(m.boolean);
    }
    CHECK(d.dispatch("typed strict true") == core::Status::ok);
    CHECK(d.dispatch("typed strict false") == core::Status::ok);
    const auto calls = m.calls;
    for (auto text :
         {"typed strict TRUE", "typed strict 1", "typed strict on",
          "typed friendly flase", "typed friendly 2", "typed friendly \"\""}) {
      CHECK(d.dispatch(text) == core::Status::invalid_argument);
    }
    CHECK(m.calls == calls);
  });
}

TEST_CASE(
    "typed diagnostics help and handler attribution use declaration metadata") {
  auto records = Run([](auto&, auto& d) {
    CHECK(d.dispatch("typed sample 2 3") == core::Status::ok);
    CHECK(d.dispatch("typed sample 2 bad") == core::Status::invalid_argument);
    CHECK(d.dispatch("typed sample") == core::Status::invalid_argument);
    CHECK(d.dispatch("typed help") == core::Status::ok);
  });
#if DAVEOS_LOGGING
  bool attributed = false, error = false, count = false, required = false,
       optional = false;
  for (const auto& record : records) {
    attributed |= record.module == "typed" && record.task == "Sample" &&
                  record.message == "sample invoked";
    error |= record.message.find("argument 'count'") != std::string::npos;
    count |=
        record.message.find("expected 1 to 2 arguments") != std::string::npos;
    required |= record.message.find("<rate>") != std::string::npos;
    optional |= record.message.find("[count]") != std::string::npos;
  }
  CHECK(attributed);
  CHECK(error);
  CHECK(count);
  CHECK(required);
  CHECK(optional);
#else
  CHECK(records.empty());
#endif
}

TEST_CASE("direct factory retains handler attribution") {
  constexpr auto descriptor =
      core::command<&Typed::Boolean>("public", "Boolean", core::arg("value"));
  STATIC_REQUIRE(std::string_view(descriptor.handler) == "Boolean");
}

TEST_CASE(
    "tokenized text and chains of optional arguments retain positional "
    "meaning") {
  Run([](auto& m, auto& d) {
    CHECK(d.dispatch(R"(typed text "hello world")") == core::Status::ok);
    CHECK(m.text == "hello world");
    CHECK_FALSE(m.tail.has_value());
    CHECK_FALSE(m.enabled.has_value());
    CHECK_FALSE(m.ratio.has_value());
    CHECK(d.dispatch(R"(typed text "" "")") == core::Status::ok);
    CHECK(m.text.empty());
    REQUIRE(m.tail.has_value());
    CHECK(m.tail->empty());
    CHECK_FALSE(m.enabled.has_value());
    CHECK(d.dispatch(R"(typed text word suffix clear)") == core::Status::ok);
    CHECK(m.tail == "suffix");
    REQUIRE(m.enabled.has_value());
    CHECK_FALSE(*m.enabled);
    CHECK_FALSE(m.ratio.has_value());
    CHECK(d.dispatch(R"(typed text "a\"b" "c\\d" high 1.25)") ==
          core::Status::ok);
    CHECK(m.text == "a\"b");
    CHECK(m.tail == "c\\d");
    CHECK(m.enabled == true);
    CHECK(m.ratio == 1.25);
    const auto calls = m.calls;
    for (auto line :
         {"typed text", "typed text word suffix bad",
          "typed text word suffix on bad", "typed text word suffix on 0.49",
          "typed text word suffix on 2.01", "typed text word suffix on 1 extra",
          R"(typed text word suffix "")"}) {
      CHECK(d.dispatch(line) == core::Status::invalid_argument);
      CHECK(m.calls == calls);
      CHECK(m.text == "a\"b");
      CHECK(m.ratio == 1.25);
    }
    // Later invocations must not reuse optional values from the previous call.
    CHECK(d.dispatch("typed text final") == core::Status::ok);
    CHECK(m.text == "final");
    CHECK_FALSE(m.tail.has_value());
    CHECK_FALSE(m.enabled.has_value());
    CHECK_FALSE(m.ratio.has_value());
  });
}

TEST_CASE(
    "all-optional handlers accept each prefix without skipping positions") {
  Run([](auto& m, auto& d) {
    CHECK(d.dispatch("typed optional") == core::Status::ok);
    CHECK_FALSE(m.optional.has_value());
    CHECK_FALSE(m.enabled.has_value());
    CHECK_FALSE(m.tail.has_value());
    CHECK(d.dispatch("typed optional 2") == core::Status::ok);
    CHECK(m.optional == 2);
    CHECK_FALSE(m.enabled.has_value());
    CHECK_FALSE(m.tail.has_value());
    CHECK(d.dispatch("typed optional 2 low") == core::Status::ok);
    CHECK(m.enabled == false);
    CHECK_FALSE(m.tail.has_value());
    CHECK(d.dispatch(R"(typed optional 3 high "")") == core::Status::ok);
    CHECK(m.optional == 3);
    CHECK(m.enabled == true);
    REQUIRE(m.tail.has_value());
    CHECK(m.tail->empty());
    const auto calls = m.calls;
    for (auto line :
         {"typed optional 0", "typed optional 4", "typed optional false",
          R"(typed optional "" true suffix)", "typed optional 1 invalid",
          "typed optional 1 on text extra"}) {
      CHECK(d.dispatch(line) == core::Status::invalid_argument);
      CHECK(m.calls == calls);
    }
    CHECK(d.dispatch("typed optional") == core::Status::ok);
    CHECK_FALSE(m.optional.has_value());
    CHECK_FALSE(m.enabled.has_value());
    CHECK_FALSE(m.tail.has_value());
  });
}
