#include "platform/stm32/console/board.hpp"

#include "core/command/command.hpp"
#include "support.hpp"

namespace core = daveos::core;
namespace test = testing;

TEST_CASE(
    "real board commands validate typed arguments before touching hardware") {
  test::Fake platform;
  test::TestModule runner;
  daveos::platform::stm32::Board<test::Event> module(platform, [](auto&) {});
  board::leds = {};
  board::writes = 0;
  auto modules = core::ModuleList{&module, &runner};
  auto scheduler = core::make_scheduler<test::Event>(platform, modules);
  core::CommandDispatcher dispatcher(modules, scheduler);
  runner.first_action = [&] {
    for (std::uint32_t i = 1; i <= 3; ++i) {
      const std::string prefix = "board led " + std::to_string(i);
      CHECK(dispatcher.dispatch(prefix + " on") == core::Status::ok);
      CHECK(board::leds[i - 1]);
      CHECK(dispatcher.dispatch(prefix + " toggle") == core::Status::ok);
      CHECK_FALSE(board::leds[i - 1]);
      CHECK(dispatcher.dispatch(prefix + " off") == core::Status::ok);
      CHECK_FALSE(board::leds[i - 1]);
    }
    const auto writes = board::writes;
    for (auto line : {"board led", "board led 1", "board led 1 on extra",
                      "board led 0 on", "board led 4 on", "board led -1 on",
                      "board led 256 on", "board led 1.5 on"}) {
      CHECK(dispatcher.dispatch(line) == core::Status::invalid_argument);
      CHECK(board::writes == writes);
    }
    for (auto line : {"board led 1 unknown", R"(board led 1 "")",
                      R"(board led 1 "on off")"}) {
      CHECK(dispatcher.dispatch(line) == core::Status::not_found);
      CHECK(board::writes == writes);
    }
    CHECK(dispatcher.dispatch("board led 1 o") ==
          core::Status::ambiguous_match);
    CHECK(board::writes == writes);
    CHECK(dispatcher.dispatch("board led 1 ON") == core::Status::ok);
    CHECK(board::leds[0]);
    CHECK(dispatcher.dispatch("board led 1 t") == core::Status::ok);
    CHECK_FALSE(board::leds[0]);
    CHECK(dispatcher.dispatch("board led 0x2 on") == core::Status::ok);
    CHECK(board::leds[1]);
    CHECK(dispatcher.dispatch("board button") == core::Status::ok);
    CHECK(dispatcher.dispatch("board stats") == core::Status::ok);
    CHECK(dispatcher.dispatch("board reset") == core::Status::unsupported);
    for (auto line : {"board button extra", "board stats extra",
                      "board reset extra", "board timer", "board timer 0",
                      "board timer -1", "board timer 10 extra"}) {
      CHECK(dispatcher.dispatch(line) == core::Status::invalid_argument);
    }
    CHECK(dispatcher.dispatch("board timer 10") == core::Status::ok);
    scheduler.stop();
  };
  REQUIRE(scheduler.schedule<&test::TestModule::first>(
              runner, std::chrono::microseconds{0}) == core::Status::ok);
  REQUIRE(scheduler.run() == core::Status::ok);
}
