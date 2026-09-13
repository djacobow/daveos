#include "support.h"
using namespace testing;

TEST_CASE(
    "logs retain call time, values and attribution; dispatch follows due "
    "work") {
  Fake platform;
  TestModule module;
  Sink sink;
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module},
                                         SubscriberList{sink.subscriber()});
  module.first_action = [&] {
    int value = 7;
    scheduler.log(Level::info, "value=%d", value);
    value = 9;
    CHECK(sink.records.empty());
    platform.advance(5);
    platform.interrupt(
        [](void* context) {
          static_cast<SchedulerInterface<Event>*>(context)->log(Level::warning,
                                                                "IRQ");
        },
        static_cast<SchedulerInterface<Event>*>(&scheduler));
    scheduler.stop();
  };
  scheduler.schedule(module, &TestModule::first, 10);
  CHECK(scheduler.run() == Status::ok);
  REQUIRE(sink.records.size() == 2);
  CHECK(sink.records[0].timestamp == 10);
  CHECK(sink.records[0].message == "value=7");
  CHECK(sink.records[0].module == "module");
  CHECK(sink.records[0].task == "first");
  CHECK(sink.records[1].timestamp == 15);
  CHECK(sink.records[1].task == "interrupt");
  CHECK(sink.records[1].severity == Level::warning);
}

TEST_CASE("filtering, truncation, overflow and reset") {
  Fake platform;
  TestModule module;
  Sink sink;
  auto scheduler = make_scheduler<Event, 32, 16, 2, 8>(
      platform, ModuleList{&module}, SubscriberList{sink.subscriber()});
  CHECK(scheduler.log(Level::debug, "filtered") == Status::ok);
  CHECK(scheduler.log(Level::info, "12345678") == Status::truncated);
  scheduler.minimum(Level::debug);
  CHECK(scheduler.log(Level::debug, "visible") == Status::ok);
  CHECK(scheduler.log(Level::error, "overflow") == Status::full);
  module.first_action = [&] { scheduler.stop(); };
  scheduler.schedule(module, &TestModule::first, 0);
  CHECK(scheduler.run() == Status::ok);
  REQUIRE(sink.records.size() == 2);
  CHECK(sink.records[0].message == "1234567");
  CHECK(sink.records[1].message == "visible");
  CHECK(scheduler.snapshot().logs.dropped == 1);
  CHECK(scheduler.snapshot().logs.truncated == 1);
  scheduler.reset_statistics();
  CHECK(scheduler.snapshot().logs.dropped == 0);
}

TEST_CASE("statistics table contains task names and diagnostic summary") {
  Fake platform;
  TestModule module;
  Sink sink;
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module},
                                         SubscriberList{sink.subscriber()});
  module.first_action = [&] { platform.advance(3); };
  module.second_action = [&] {
    scheduler.log_statistics();
    scheduler.stop();
  };
  scheduler.schedule(module, &TestModule::first, 1);
  scheduler.schedule(module, &TestModule::second, 10);
  CHECK(scheduler.run() == Status::ok);
  REQUIRE(sink.records.size() == 5);
  CHECK(sink.records[1].message.find("module/first") != std::string::npos);
  CHECK(sink.records.back().message.find("Overflow events=0") !=
        std::string::npos);
}

TEST_CASE("no subscriber discards logs and fatal does not stop") {
  Fake platform;
  TestModule module;
  auto scheduler =
      make_scheduler<Event, 32, 16, 1>(platform, ModuleList{&module});
  module.first_action = [&] {
    for (int index = 0; index < 5; ++index)
      CHECK(scheduler.log(Level::fatal, "fatal") == Status::ok);
    scheduler.schedule(module, &TestModule::second, 1);
  };
  module.second_action = [&] { scheduler.stop(); };
  scheduler.schedule(module, &TestModule::first, 0);
  CHECK(scheduler.run() == Status::ok);
  CHECK(scheduler.snapshot().logs.dropped == 0);
  CHECK(scheduler.snapshot().tasks[1].executions == 1);
}
