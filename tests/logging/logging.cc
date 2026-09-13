#include "support.h"
using namespace testing;

TEST_CASE("severity macros preserve arguments status and deferred delivery") {
  struct LoggingModule : Module<LoggingModule, Event> {
    static constexpr const char* name() { return "macro_module"; }
    int calls = 0;
    static constexpr auto tasks() {
      return std::array{
          TaskDescriptor<LoggingModule>{"emit", &LoggingModule::emit}};
    }
    void emit() {
      CHECK(D_("value=%d", ++calls) == Status::ok);
      CHECK(I_("info") == Status::ok);
      CHECK(W_("warning") == Status::ok);
      CHECK(E_("error") == Status::ok);
      CHECK(F_("fatal") == Status::ok);
      CHECK(calls == 1);
      scheduler().stop();
    }
  } module;
  Fake platform;
  Sink sink;
  auto logger = make_logger(platform, SubscriberList{sink.subscriber()});
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module}, logger);
  logger.minimum(Level::debug);
  scheduler.schedule(module, &LoggingModule::emit, 10);
  CHECK(sink.records.empty());
  REQUIRE(scheduler.run() == Status::ok);
  REQUIRE(sink.records.size() == 5);
  CHECK(sink.records[0].message == "value=1");
  const auto levels = std::array{Level::debug, Level::info, Level::warning,
                                 Level::error, Level::fatal};
  for (std::size_t i = 0; i < levels.size(); ++i) {
    CHECK(sink.records[i].severity == levels[i]);
    CHECK(sink.records[i].module == "macro_module");
    CHECK(sink.records[i].task == "emit");
    CHECK(sink.records[i].timestamp == 10);
  }
}

TEST_CASE(
    "logs retain call time, values and attribution; dispatch follows due "
    "work") {
  Fake platform;
  TestModule module;
  Sink sink;
  auto logger = make_logger(platform, SubscriberList{sink.subscriber()});
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module}, logger);
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
  auto logger = make_logger<2, 8>(platform, SubscriberList{sink.subscriber()});
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module}, logger);
  CHECK(scheduler.log(Level::debug, "filtered") == Status::ok);
  CHECK(scheduler.log(Level::info, "12345678") == Status::truncated);
  logger.minimum(Level::debug);
  CHECK(scheduler.log(Level::debug, "visible") == Status::ok);
  CHECK(scheduler.log(Level::error, "overflow") == Status::full);
  module.first_action = [&] { scheduler.stop(); };
  scheduler.schedule(module, &TestModule::first, 0);
  CHECK(scheduler.run() == Status::ok);
  REQUIRE(sink.records.size() == 2);
  CHECK(sink.records[0].message == "1234567");
  CHECK(sink.records[1].message == "visible");
  CHECK(logger.counters().dropped == 1);
  CHECK(logger.counters().truncated == 1);
  scheduler.reset_statistics();
  CHECK(logger.counters().dropped == 1);
  logger.reset();
  CHECK(logger.counters().dropped == 0);
}

TEST_CASE("statistics table contains task names and diagnostic summary") {
  Fake platform;
  TestModule module;
  Sink sink;
  auto logger = make_logger(platform, SubscriberList{sink.subscriber()});
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module}, logger);
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

TEST_CASE("no logger discards logs and fatal does not stop") {
  Fake platform;
  TestModule module;
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module});
  module.first_action = [&] {
    for (int index = 0; index < 5; ++index)
      CHECK(scheduler.log(Level::fatal, "fatal") == Status::ok);
    scheduler.schedule(module, &TestModule::second, 1);
  };
  module.second_action = [&] { scheduler.stop(); };
  scheduler.schedule(module, &TestModule::first, 0);
  CHECK(scheduler.run() == Status::ok);
  CHECK(scheduler.snapshot().tasks[1].executions == 1);
}

TEST_CASE("idle delivery yields to work scheduled by a subscriber") {
  Fake platform;
  TestModule module;
  struct Delivery {
    SchedulerInterface<Event>* scheduler = nullptr;
    TestModule* module;
    std::vector<std::string> order;
  } delivery{nullptr, &module, {}};
  Subscriber sink{&delivery, [](void* pointer, const LogRecord& record) {
                    auto& d = *static_cast<Delivery*>(pointer);
                    d.order.emplace_back(record.message);
                    if (d.order.size() == 1)
                      d.scheduler->schedule(*d.module, &TestModule::second, 0);
                    else
                      d.scheduler->stop();
                  }};
  auto logger = make_logger(platform, SubscriberList{sink});
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module}, logger);
  delivery.scheduler = &scheduler;
  module.first_action = [&] {
    scheduler.log(Level::info, "first");
    scheduler.log(Level::info, "second");
  };
  module.second_action = [&] { delivery.order.emplace_back("task"); };
  scheduler.schedule(module, &TestModule::first, 0);
  REQUIRE(scheduler.run() == Status::ok);
  CHECK(delivery.order == std::vector<std::string>{"first", "task", "second"});
  CHECK(platform.sleeps() == 0);
}

TEST_CASE("logging during the sleep decision prevents sleep") {
  struct ModuleWithIdleLog : Module<ModuleWithIdleLog, Event> {
    static constexpr const char* name() { return "idle_log"; }
    bool can_sleep() {
      scheduler().log(Level::info, "arrived before sleep");
      return true;
    }
  } module;
  Fake platform;
  SchedulerInterface<Event>* interface = nullptr;
  unsigned delivered = 0;
  struct Delivery {
    SchedulerInterface<Event>** scheduler;
    unsigned* count;
  } d{&interface, &delivered};
  auto logger = make_logger(
      platform,
      SubscriberList{Subscriber{&d, [](void* pointer, const LogRecord&) {
                                  auto& d = *static_cast<Delivery*>(pointer);
                                  ++*d.count;
                                  (*d.scheduler)->stop();
                                }}});
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module}, logger);
  interface = &scheduler;
  REQUIRE(scheduler.run() == Status::ok);
  CHECK(delivered == 1);
  CHECK(platform.sleeps() == 0);
}

TEST_CASE("interrupt enqueue retains a notification for idle dispatch") {
  Fake platform;
  TestModule module;
  auto logger = make_logger(
      platform,
      SubscriberList{Subscriber{nullptr, [](void*, const LogRecord&) {}}});
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module}, logger);
  struct Enqueue {
    Fake& platform;
    SchedulerInterface<Event>& scheduler;
    bool notified = false;
  } enqueue{platform, scheduler};
  REQUIRE(platform.interrupt(
              [](void* pointer) {
                auto& e = *static_cast<Enqueue*>(pointer);
                auto before = e.platform.sequence();
                CHECK(e.scheduler.log(Level::info, "IRQ") == Status::ok);
                e.notified = e.platform.sequence() != before;
              },
              &enqueue) == Status::ok);
  CHECK(enqueue.notified);  // observed before interrupt()'s own notification
  CHECK_FALSE(logger.empty());
  CHECK(logger.dispatch());
  CHECK(logger.empty());
}

TEST_CASE("logger platform mismatch fails before module initialization") {
  Fake platform, other;
  TestModule module;
  Sink sink;
  auto logger = make_logger(other, SubscriberList{sink.subscriber()});
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module}, logger);
  bool initialized = false, ran = false;
  module.initializer = [&](InitStage) {
    initialized = true;
    return Status::ok;
  };
  module.first_action = [&] { ran = true; };
  module.receiver = [&](Event) { ran = true; };
  CHECK(scheduler.schedule(module, &TestModule::first, 0) == Status::ok);
  CHECK(scheduler.post(Event::first) == Status::ok);
  CHECK(scheduler.log(Level::info, "before init") == Status::ok);
  SECTION("explicit init") {
    CHECK(scheduler.init() == Status::invalid_argument);
  }
  SECTION("automatic init") {}
  CHECK(scheduler.run() == Status::invalid_argument);
  CHECK(scheduler.init() == Status::invalid_argument);
  CHECK_FALSE(initialized);
  CHECK_FALSE(ran);
  CHECK(scheduler.post(Event::first) == Status::not_running);
  REQUIRE(sink.records.size() == 1);
  CHECK(sink.records[0].message == "before init");
}
