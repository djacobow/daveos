#include "console/transport.hpp"
#include "core/schedule/application.hpp"
#include "platform/stm32/console/console.hpp"
#include "support.hpp"

namespace core = daveos::core;
namespace console = daveos::console;
namespace stm32 = daveos::platform::stm32;
namespace test = testing;

namespace {
  struct State {
    bool initialized = false;
    std::uint32_t output = 0;
    std::vector<int>& stops;
    int id;
  };

  template <test::TestName Name>
  struct Transport {
    State& state;

    explicit Transport(State& value) : state(value) {}

    static constexpr const char* name() { return Name.value; }

    static constexpr const char* statistics_label() { return Name.value; }

    bool init() {
      state.initialized = true;
      return true;
    }

    void stop() { state.stops.push_back(state.id); }

    bool poll_line(console::Line&) { return false; }

    std::uint32_t take_dropped() { return 0; }

    void output(const core::LogRecord&) { ++state.output; }

    console::TxCounters counters() { return {}; }
  };
}  // namespace

TEST_CASE(
    "console group registers transports independently and stops in reverse "
    "order") {
  test::Fake platform;
  std::vector<int> stops;
  State first{false, 0, stops, 1}, second{false, 0, stops, 2};
  console::TransportModule<test::Event, Transport<"first">> one(first);
  console::TransportModule<test::Event, Transport<"second">> two(second);
  stm32::Console group{one, two};
  CHECK_FALSE(first.initialized);
  CHECK_FALSE(second.initialized);
  test::TestModule controller;
  auto logger = core::make_logger(platform, group.subscribers());
  auto app = core::make_application<test::Event>(
      platform, group.modules(controller), logger, group.sources());
  controller.first_action = [&] {
    CHECK(first.initialized);
    CHECK(second.initialized);
    CHECK(one.command_source().dispatch("help") == core::Status::ok);
    CHECK(two.command_source().dispatch("help") == core::Status::ok);
    group.log_statistics(app.scheduler());
    CHECK(app.scheduler().stop() == core::Status::ok);
  };
  CHECK(app.scheduler().schedule<&test::TestModule::first>(
            controller, std::chrono::microseconds{1}) == core::Status::ok);
  CHECK(app.run() == core::Status::ok);
  CHECK(first.output == second.output);
  CHECK((first.output > 0) == bool(DAVEOS_LOGGING));
  group.stop();
  CHECK(stops == std::vector<int>{2, 1});
}

TEST_CASE(
    "empty console group leaves ordinary modules usable without logging or "
    "inputs") {
  test::Fake platform;
  test::TestModule module;
  stm32::Console group{};
  auto app =
      core::make_application<test::Event>(platform, group.modules(module));
  STATIC_REQUIRE(std::tuple_size_v<decltype(group.sources().items)> == 0);
  module.first_action = [&] {
    CHECK(app.scheduler().stop() == core::Status::ok);
  };
  CHECK(app.scheduler().schedule<&test::TestModule::first>(
            module, std::chrono::microseconds{0}) == core::Status::ok);
  CHECK(app.run() == core::Status::ok);
  group.stop();
}
