#include "core/logging/log_format.hpp"
#include "support.hpp"

namespace core = daveos::core;
namespace test = testing;

TEST_CASE("severity macros preserve arguments status and deferred delivery") {
  struct LoggingModule : core::Module<LoggingModule, test::Event> {
    static constexpr const char* name() { return "macro_module"; }

    int calls = 0;

    static constexpr auto tasks() {
      return std::array{
          core::TaskDescriptor<LoggingModule>{"emit", &LoggingModule::emit}};
    }

    void emit() {
      CHECK(D_("value=%d", ++calls) == core::Status::ok);
      CHECK(I_("info") == core::Status::ok);
      CHECK(W_("warning") == core::Status::ok);
      CHECK(E_("error") == core::Status::ok);
      CHECK(F_("fatal") == core::Status::ok);
      CHECK(calls == 1);
      scheduler().stop();
    }
  } module;

  test::Fake platform;
  test::Sink sink;
  auto logger =
      core::make_logger(platform, core::SubscriberList{sink.subscriber()});
  auto scheduler = core::make_scheduler<test::Event>(
      platform, core::ModuleList{&module}, logger);
  logger.minimum(core::Level::debug);
  CHECK(scheduler.schedule<&LoggingModule::emit>(
            module, std::chrono::microseconds{10}) == core::Status::ok);
  CHECK(sink.records.empty());
  REQUIRE(scheduler.run() == core::Status::ok);
  REQUIRE(sink.records.size() == 5);
  CHECK(sink.records[0].message == "value=1");
  const auto levels =
      std::array{core::Level::debug, core::Level::info, core::Level::warning,
                 core::Level::error, core::Level::fatal};
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
  test::Fake platform;
  test::TestModule module;
  test::Sink sink;
  auto logger =
      core::make_logger(platform, core::SubscriberList{sink.subscriber()});
  auto scheduler = core::make_scheduler<test::Event>(
      platform, core::ModuleList{&module}, logger);
  module.first_action = [&] {
    int value = 7;
    scheduler.log(core::Level::info, "value=%d", value);
    value = 9;
    CHECK(sink.records.empty());
    platform.advance(5);
    platform.interrupt(
        [](void* context) {
          static_cast<core::SchedulerInterface<test::Event>*>(context)->log(
              core::Level::warning, "IRQ");
        },
        static_cast<core::SchedulerInterface<test::Event>*>(&scheduler));
    scheduler.stop();
  };
  CHECK(scheduler.schedule<&test::TestModule::first>(
            module, std::chrono::microseconds{10}) == core::Status::ok);
  CHECK(scheduler.run() == core::Status::ok);
  REQUIRE(sink.records.size() == 2);
  CHECK(sink.records[0].timestamp == 10);
  CHECK(sink.records[0].message == "value=7");
  CHECK(sink.records[0].module == "module");
  CHECK(sink.records[0].task == "first");
  CHECK(sink.records[1].timestamp == 15);
  CHECK(sink.records[1].task == "interrupt");
  CHECK(sink.records[1].severity == core::Level::warning);
}

TEST_CASE("filtering, truncation, overflow and reset") {
  test::Fake platform;
  test::TestModule module;
  test::Sink sink;
  auto logger = core::make_logger<2, 8>(
      platform, core::SubscriberList{sink.subscriber()});
  auto scheduler = core::make_scheduler<test::Event>(
      platform, core::ModuleList{&module}, logger);
  CHECK(scheduler.log(core::Level::debug, "filtered") == core::Status::ok);
  CHECK(scheduler.log(core::Level::info, "12345678") ==
        core::Status::truncated);
  logger.minimum(core::Level::debug);
  CHECK(scheduler.log(core::Level::debug, "visible") == core::Status::ok);
  CHECK(scheduler.log(core::Level::error, "overflow") == core::Status::full);
  module.first_action = [&] { scheduler.stop(); };
  CHECK(scheduler.schedule<&test::TestModule::first>(
            module, std::chrono::microseconds{0}) == core::Status::ok);
  CHECK(scheduler.run() == core::Status::ok);
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
  test::Fake platform;
  test::TestModule module;
  test::Sink sink;
  auto logger =
      core::make_logger(platform, core::SubscriberList{sink.subscriber()});
  auto scheduler = core::make_scheduler<test::Event>(
      platform, core::ModuleList{&module}, logger);
  module.first_action = [&] { platform.advance(3); };
  module.second_action = [&] {
    scheduler.log_statistics();
    scheduler.stop();
  };
  CHECK(scheduler.schedule<&test::TestModule::first>(
            module, std::chrono::microseconds{1}) == core::Status::ok);
  CHECK(scheduler.schedule<&test::TestModule::second>(
            module, std::chrono::microseconds{10}) == core::Status::ok);
  CHECK(scheduler.run() == core::Status::ok);
  REQUIRE(sink.records.size() == 5);
  CHECK(sink.records[1].message.find("module/first") != std::string::npos);
  CHECK(sink.records.back().message.find("Overflow events=0") !=
        std::string::npos);
}

TEST_CASE("no logger discards logs and fatal does not stop") {
  test::Fake platform;
  test::TestModule module;
  auto scheduler =
      core::make_scheduler<test::Event>(platform, core::ModuleList{&module});
  module.first_action = [&] {
    for (int index = 0; index < 5; ++index) {
      CHECK(scheduler.log(core::Level::fatal, "fatal") == core::Status::ok);
    }
    CHECK(scheduler.schedule<&test::TestModule::second>(
              module, std::chrono::microseconds{1}) == core::Status::ok);
  };
  module.second_action = [&] { scheduler.stop(); };
  CHECK(scheduler.schedule<&test::TestModule::first>(
            module, std::chrono::microseconds{0}) == core::Status::ok);
  CHECK(scheduler.run() == core::Status::ok);
  CHECK(scheduler.snapshot().tasks[1].executions == 1);
}

TEST_CASE("idle delivery yields to work scheduled by a subscriber") {
  test::Fake platform;
  test::TestModule module;

  struct Delivery {
    core::SchedulerInterface<test::Event>* scheduler = nullptr;
    test::TestModule* module;
    std::vector<std::string> order;
  } delivery{nullptr, &module, {}};

  core::Subscriber sink{
      &delivery, [](void* pointer, const core::LogRecord& record) {
        auto& d = *static_cast<Delivery*>(pointer);
        d.order.emplace_back(record.message);
        if (d.order.size() == 1) {
          CHECK(d.scheduler->schedule<&test::TestModule::second>(
                    *d.module, std::chrono::microseconds{0}) ==
                core::Status::ok);
        } else {
          d.scheduler->stop();
        }
      }};
  auto logger = core::make_logger(platform, core::SubscriberList{sink});
  auto scheduler = core::make_scheduler<test::Event>(
      platform, core::ModuleList{&module}, logger);
  delivery.scheduler = &scheduler;
  module.first_action = [&] {
    scheduler.log(core::Level::info, "first");
    scheduler.log(core::Level::info, "second");
  };
  module.second_action = [&] { delivery.order.emplace_back("task"); };
  CHECK(scheduler.schedule<&test::TestModule::first>(
            module, std::chrono::microseconds{0}) == core::Status::ok);
  REQUIRE(scheduler.run() == core::Status::ok);
  CHECK(delivery.order == std::vector<std::string>{"first", "task", "second"});
  CHECK(platform.sleeps() == 0);
}

TEST_CASE("logging during the sleep decision prevents sleep") {
  struct ModuleWithIdleLog : core::Module<ModuleWithIdleLog, test::Event> {
    static constexpr const char* name() { return "idle_log"; }

    bool can_sleep() {
      scheduler().log(core::Level::info, "arrived before sleep");
      return true;
    }
  } module;

  test::Fake platform;
  core::SchedulerInterface<test::Event>* interface = nullptr;
  unsigned delivered = 0;

  struct Delivery {
    core::SchedulerInterface<test::Event>** scheduler;
    unsigned* count;
  } d{&interface, &delivered};

  auto logger = core::make_logger(
      platform, core::SubscriberList{core::Subscriber{
                    &d, [](void* pointer, const core::LogRecord&) {
                      auto& d = *static_cast<Delivery*>(pointer);
                      ++*d.count;
                      (*d.scheduler)->stop();
                    }}});
  auto scheduler = core::make_scheduler<test::Event>(
      platform, core::ModuleList{&module}, logger);
  interface = &scheduler;
  REQUIRE(scheduler.run() == core::Status::ok);
  CHECK(delivered == 1);
  CHECK(platform.sleeps() == 0);
}

TEST_CASE("interrupt enqueue retains a notification for idle dispatch") {
  test::Fake platform;
  test::TestModule module;
  auto logger = core::make_logger(
      platform, core::SubscriberList{core::Subscriber{
                    nullptr, [](void*, const core::LogRecord&) {}}});
  auto scheduler = core::make_scheduler<test::Event>(
      platform, core::ModuleList{&module}, logger);

  struct Enqueue {
    test::Fake& platform;
    core::SchedulerInterface<test::Event>& scheduler;
    bool notified = false;
  } enqueue{platform, scheduler};

  REQUIRE(platform.interrupt(
              [](void* pointer) {
                auto& e = *static_cast<Enqueue*>(pointer);
                auto before = e.platform.sequence();
                CHECK(e.scheduler.log(core::Level::info, "IRQ") ==
                      core::Status::ok);
                e.notified = e.platform.sequence() != before;
              },
              &enqueue) == core::Status::ok);
  CHECK(enqueue.notified);  // observed before interrupt()'s own notification
  CHECK_FALSE(logger.empty());
  CHECK(logger.dispatch());
  CHECK(logger.empty());
}

TEST_CASE("logger platform mismatch fails before module initialization") {
  test::Fake platform, other;
  test::TestModule module;
  test::Sink sink;
  auto logger =
      core::make_logger(other, core::SubscriberList{sink.subscriber()});
  auto scheduler = core::make_scheduler<test::Event>(
      platform, core::ModuleList{&module}, logger);
  bool initialized = false, ran = false;
  module.initializer = [&](core::InitStage) {
    initialized = true;
    return core::Status::ok;
  };
  module.first_action = [&] { ran = true; };
  module.receiver = [&](test::Event) { ran = true; };
  CHECK(scheduler.schedule<&test::TestModule::first>(
            module, std::chrono::microseconds{0}) == core::Status::ok);
  CHECK(scheduler.post(test::First{}) == core::Status::ok);
  CHECK(scheduler.log(core::Level::info, "before init") == core::Status::ok);
  SECTION("explicit init") {
    CHECK(scheduler.init() == core::Status::invalid_argument);
  }
  SECTION("automatic init") {}
  CHECK(scheduler.run() == core::Status::invalid_argument);
  CHECK(scheduler.init() == core::Status::invalid_argument);
  CHECK_FALSE(initialized);
  CHECK_FALSE(ran);
  CHECK(scheduler.initialization_failure().status ==
        core::Status::invalid_argument);
  CHECK(scheduler.initialization_failure().module == nullptr);
  CHECK(scheduler.post(test::First{}) == core::Status::not_running);
  REQUIRE(sink.records.size() == 2);
  CHECK(sink.records[0].message == "before init");
  CHECK(sink.records[1].message ==
        "Initialization failed: invalid_argument (registration, stage 0)");
}

TEST_CASE("log prefixes split elapsed time and align bounded context") {
  core::LogRecord record{
      ((core::Time{12} * 24 + 3) * 3600 + 4 * 60 + 5) * 1000000 + 678999,
      core::Level::info, "board", "Button", "message"};
  core::LogPrefix prefix(record);
  CHECK(prefix.view() == "[012:03:04:05.678] I board.Button          : ");
  core::LogPrefix<8> short_prefix(record);
  CHECK(short_prefix.view() == "[012:03:04:05.678] I board...: ");
  record.timestamp = core::Time{1000} * 24 * 3600 * 1000000;
  CHECK(core::LogPrefix(record).view().starts_with("[1000:00:00:00.000] I "));
  record.timestamp = core::kForever;
  CHECK(core::LogPrefix(record).view().starts_with(
      "[213503982:08:01:49.551] I "));
}

TEST_CASE("integer log display avoids 64-bit printf and marks capped values") {
  CHECK(std::string_view(core::LogUnsigned(0).c_str()) == "0");
  CHECK(std::string_view(core::LogUnsigned(4294967295ULL).c_str()) ==
        "4294967295");
  CHECK(std::string_view(core::LogUnsigned(4294967296ULL).c_str()) ==
        "4294967295+");
  CHECK(std::string_view(core::LogUnsigned(core::kForever).c_str()) ==
        "4294967295+");
}

TEST_CASE("average log display uses bounded integer arithmetic") {
  const auto display = [](std::uint64_t total, std::uint64_t count) {
    return std::string(core::LogAverage(total, count).c_str());
  };
  CHECK(display(0, 0) == "0.0");
  CHECK(display(1, 3) == "0.3");
  CHECK(display(2, 3) == "0.7");
  CHECK(display(25, 20) == "1.3");
  CHECK(display(199, 100) == "2.0");
  CHECK(display(UINT64_MAX, UINT64_MAX) == "1.0");
  CHECK(display(UINT64_MAX - 1, UINT64_MAX) == "1.0");
  CHECK(display(UINT64_MAX / 2, UINT64_MAX) == "0.5");
  CHECK(display(UINT64_MAX, 1) == "4294967295+");
  CHECK(display(UINT32_MAX, 1) == "4294967295.0");
  CHECK(display(UINT64_C(42949672959), 10) == "4294967295+");
  for (std::uint64_t count = 1; count <= 101; ++count) {
    for (std::uint64_t total = 0; total <= 303; ++total) {
      const auto tenths = (total * 20 + count) / (count * 2);
      CHECK(display(total, count) ==
            std::to_string(tenths / 10) + "." + std::to_string(tenths % 10));
    }
  }
}
