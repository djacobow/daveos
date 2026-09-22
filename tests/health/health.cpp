#include "watchdog/health.hpp"

#include <cstdlib>

#include "core/command/command.hpp"
#include "support.hpp"

namespace core = daveos::core;
namespace util = daveos::util;
namespace test = testing;

namespace {
  // Board reliability services modelled in memory.
  struct Hardware {
    struct Watchdog {
      daveos::watchdog::Driver driver() {
        return {nullptr,
                [](void*, core::Time timeout) {
                  started = timeout;
                  return core::Status::ok;
                },
                [](void*) {
                  ++feeds;
                  return core::Status::ok;
                }};
      }
    };

    static inline core::Time started = 0;
    static inline int feeds = 0;
    static inline bool previous_reset = false;
    static inline util::fault::Record record{};

    static bool prepare_health(const util::Version&) { return previous_reset; }

    [[noreturn]] static void initialization_failed() { std::abort(); }

    static util::fault::Record& retained_fault() { return record; }

    static void record_failure(util::fault::Data data) {
      util::fault::save(record, data);
    }

    static util::crc32::Backend crc32_backend() { return {}; }

    static void Reset() {
      started = 0;
      feeds = 0;
      previous_reset = false;
      util::fault::clear(record);
    }
  };

  using Health =
      daveos::watchdog::HealthModule<test::Event, test::Fake, Hardware>;
  constexpr util::Version kVersion{1, 2, 3};

  bool Logged(const std::vector<test::Record>& records, std::string_view text) {
    return std::any_of(records.begin(), records.end(),
                       [&](const auto& r) { return r.message == text; });
  }
}  // namespace

TEST_CASE("health module starts the watchdog, feeds it and confirms an image") {
  Hardware::Reset();
  Hardware::previous_reset = true;
  test::Fake platform;
  Health health(platform, kVersion);
  test::TestModule control;
  test::Sink sink;
  auto modules = core::ModuleList{&health, &control};
  auto logger =
      core::make_logger(platform, core::SubscriberList{sink.subscriber()});
  auto scheduler = core::make_scheduler<test::Event>(platform, modules, logger);
  int confirmations = 0;
  health.confirmation(&confirmations, [](void* p) {
    ++*static_cast<int*>(p);
    return core::Status::ok;
  });
  health.early(scheduler);
  CHECK(Hardware::started == 5000000);
  control.first_action = [&] { scheduler.stop(); };
  REQUIRE(scheduler.schedule<&test::TestModule::first>(
              control, std::chrono::seconds{6}) == core::Status::ok);
  REQUIRE(scheduler.run() == core::Status::ok);
  CHECK(Hardware::feeds > 0);
  CHECK(confirmations == 1);
#if DAVEOS_LOGGING
  CHECK(Logged(sink.records, "Previous reset: IWDG"));
  CHECK(Logged(sink.records, "No retained failure"));
  CHECK(Logged(sink.records, "Healthy for five seconds; image confirmed"));
  CHECK(
      std::all_of(sink.records.begin(), sink.records.end(), [](const auto& r) {
        return r.module == "health" || r.module == "core";
      }));
#endif
}

TEST_CASE("health commands report, verify CRC and clear retained failures") {
  Hardware::Reset();
  util::fault::Data data;
  data.kind = util::fault::Kind::watchdog;
  data.status = static_cast<std::uint32_t>(core::Status::health_failed);
  util::fault::copy_name(data.check, "task_progress");
  util::fault::copy_name(data.module, "sensor");
  util::fault::copy_name(data.task, "Poll");
  Hardware::record_failure(data);
  test::Fake platform;
  Health health(platform, kVersion);
  test::TestModule control;
  test::Sink sink;
  auto modules = core::ModuleList{&health, &control};
  auto logger =
      core::make_logger(platform, core::SubscriberList{sink.subscriber()});
  auto scheduler = core::make_scheduler<test::Event>(platform, modules, logger);
  core::CommandDispatcher<test::Event, decltype(modules)> dispatcher{modules,
                                                                     scheduler};
  health.early(scheduler);
  control.first_action = [&] {
    CHECK(dispatcher.dispatch("health status") == core::Status::ok);
    CHECK(dispatcher.dispatch("health crc hello") == core::Status::ok);
    CHECK(dispatcher.dispatch("health clear") == core::Status::ok);
    CHECK(dispatcher.dispatch("health fault") == core::Status::ok);
    scheduler.stop();
  };
  REQUIRE(scheduler.schedule<&test::TestModule::first>(
              control, std::chrono::milliseconds{1}) == core::Status::ok);
  REQUIRE(scheduler.run() == core::Status::ok);
  util::fault::Data after;
  CHECK_FALSE(util::fault::read(Hardware::record, after));
#if DAVEOS_LOGGING
  // Stage2 reports the failure retained from before this "boot".
  CHECK(Logged(sink.records,
               "Retained failure: task_progress sensor.Poll status "
               "health_failed"));
  CHECK(Logged(sink.records, "Watchdog running; confirmation not configured"));
  CHECK(Logged(sink.records,
               "CRC32 3610a686 (hardware/software/incremental agree)"));
  CHECK(Logged(sink.records, "Retained failure cleared"));
  CHECK(Logged(sink.records, "No retained failure"));
#endif
}
