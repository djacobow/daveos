#include "support.hpp"

namespace core = daveos::core;
namespace test = testing;

TEST_CASE("disabled macros do not evaluate or require their arguments") {
  static_assert(!DAVEOS_LOGGING);
  int calls = 0;
  CHECK(D_("%d", ++calls) == core::Status::ok);
  CHECK(I_("%d", ++calls) == core::Status::ok);
  CHECK(W_("%d", ++calls) == core::Status::ok);
  CHECK(E_("%d", ++calls) == core::Status::ok);
  CHECK(F_("%d", ++calls) == core::Status::ok);
  // Even an unavailable target/function must disappear at preprocessing time.
  CHECK(DAVEOS_LOG(unavailable_target(), core::Level::info, "%d", missing()) ==
        core::Status::ok);
  CHECK(calls == 0);
  core::SchedulerInterface<test::Event> interface;
  CHECK(interface.log(core::Level::info, "%d", ++calls) == core::Status::ok);
  CHECK(calls == 1);  // ordinary direct-call argument evaluation still applies
}

TEST_CASE("disabled logging owns no storage and attaches no service") {
  test::Fake platform;

  struct UnusedPlatform {
  } unused_logger_platform;

  test::TestModule module;
  test::Sink sink;
  auto logger = core::make_logger<4096, 1024>(
      unused_logger_platform, core::SubscriberList{sink.subscriber()});
  STATIC_REQUIRE(std::is_empty_v<decltype(logger)>);
  auto scheduler = core::make_scheduler<test::Event>(
      platform, core::ModuleList{&module}, logger);
  using WithoutLogger = decltype(core::make_scheduler<test::Event>(
      platform, core::ModuleList{&module}));
  STATIC_REQUIRE(std::is_same_v<decltype(scheduler), WithoutLogger>);
  CHECK(scheduler.log(core::Level::fatal, "discarded") == core::Status::ok);
  module.first_action = [&] {
    scheduler.log_statistics();
    scheduler.stop();
  };
  REQUIRE(scheduler.schedule<&test::TestModule::first>(
              module, std::chrono::microseconds{0}) == core::Status::ok);
  REQUIRE(scheduler.run() == core::Status::ok);
  CHECK(sink.records.empty());
  CHECK(logger.counters().dropped == 0);
}
