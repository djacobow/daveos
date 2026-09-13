#include "support.hpp"
using namespace testing;

TEST_CASE("disabled macros do not evaluate or require their arguments") {
  static_assert(!DAVEOS_LOGGING);
  int calls = 0;
  CHECK(D_("%d", ++calls) == Status::ok);
  CHECK(I_("%d", ++calls) == Status::ok);
  CHECK(W_("%d", ++calls) == Status::ok);
  CHECK(E_("%d", ++calls) == Status::ok);
  CHECK(F_("%d", ++calls) == Status::ok);
  // Even an unavailable target/function must disappear at preprocessing time.
  CHECK(DAVEOS_LOG(unavailable_target(), Level::info, "%d", missing()) ==
        Status::ok);
  CHECK(calls == 0);
  SchedulerInterface<Event> interface;
  CHECK(interface.log(Level::info, "%d", ++calls) == Status::ok);
  CHECK(calls == 1);  // ordinary direct-call argument evaluation still applies
}

TEST_CASE("disabled logging owns no storage and attaches no service") {
  Fake platform, unused_logger_platform;
  TestModule module;
  Sink sink;
  auto logger = make_logger<4096, 1024>(unused_logger_platform,
                                        SubscriberList{sink.subscriber()});
  STATIC_REQUIRE(std::is_empty_v<decltype(logger)>);
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module}, logger);
  using WithoutLogger =
      decltype(make_scheduler<Event>(platform, ModuleList{&module}));
  STATIC_REQUIRE(std::is_same_v<decltype(scheduler), WithoutLogger>);
  CHECK(scheduler.log(Level::fatal, "discarded") == Status::ok);
  module.first_action = [&] {
    scheduler.log_statistics();
    scheduler.stop();
  };
  scheduler.schedule(module, &TestModule::first, 0);
  REQUIRE(scheduler.run() == Status::ok);
  CHECK(sink.records.empty());
  CHECK(logger.counters().dropped == 0);
}
