#include <memory>
#include <semaphore>
#include <thread>

#include "support.hpp"

namespace core = daveos::core;
namespace test = testing;

TEST_CASE("scheduler construction does not require constructed modules") {
  test::Fake platform;
  alignas(test::TestModule) std::byte storage[sizeof(test::TestModule)];
  auto* module = reinterpret_cast<test::TestModule*>(storage);
  // The scheduler may retain addresses and static descriptors, but must not
  // touch the module until init. This deliberately forms a pointer to aligned
  // storage before the object lifetime starts; construct_at starts that
  // lifetime before init() dereferences it. This models construction across
  // TUs.
  auto scheduler =
      core::make_scheduler<test::Event>(platform, core::ModuleList{module});
  auto destroy = [](test::TestModule* p) { std::destroy_at(p); };
  std::unique_ptr<test::TestModule, decltype(destroy)> owned(
      std::construct_at(module), destroy);
  std::vector<core::InitStage> stages;
  owned->initializer = [&](core::InitStage stage) {
    CHECK(&owned->scheduler() == &scheduler);
    stages.push_back(stage);
    return core::Status::ok;
  };
  CHECK(scheduler.init() == core::Status::ok);
  CHECK(stages ==
        std::vector{core::InitStage::stage1, core::InitStage::stage2});
}

TEST_CASE("startup is two passes; task deadlines start at run") {
  test::Fake platform;
  test::NamedModule<"one"> one;
  test::NamedModule<"two"> two;
  std::vector<int> stages;
  auto scheduler =
      core::make_scheduler<test::Event>(platform, core::ModuleList{&one, &two});
  one.initializer = [&](core::InitStage stage) {
    stages.push_back(stage == core::InitStage::stage1 ? 1 : 3);
    if (stage == core::InitStage::stage1) {
      CHECK(&one.scheduler() == &scheduler);
      CHECK(scheduler.schedule(one, &decltype(one)::first, 10) ==
            core::Status::ok);
      platform.advance(100);
    }
    return core::Status::ok;
  };
  two.initializer = [&](core::InitStage stage) {
    stages.push_back(stage == core::InitStage::stage1 ? 2 : 4);
    return core::Status::ok;
  };
  core::Time fired = 0;
  one.first_action = [&] {
    fired = platform.now();
    scheduler.stop();
  };
  CHECK(scheduler.stop() == core::Status::not_running);
  CHECK(scheduler.init() == core::Status::ok);
  CHECK(stages == std::vector<int>{1, 2, 3, 4});
  CHECK(scheduler.init() == core::Status::already_initialized);
  platform.advance(500);
  CHECK(scheduler.run() == core::Status::ok);
  CHECK(fired == 610);
  CHECK(scheduler.snapshot().tasks[0].late_starts == 0);
  CHECK(scheduler.run() == core::Status::already_run);
  CHECK(scheduler.stop() == core::Status::not_running);
}

TEST_CASE("initialization failure discards work and flushes diagnostics") {
  test::Fake platform;
  test::TestModule module;
  test::NamedModule<"later"> later;
  test::Sink sink;
  bool ran = false;
  auto logger =
      core::make_logger(platform, core::SubscriberList{sink.subscriber()});
  auto scheduler = core::make_scheduler<test::Event>(
      platform, core::ModuleList{&module, &later}, logger);
  module.first_action = [&] { ran = true; };
  module.initializer = [&](core::InitStage) {
    scheduler.schedule(module, &test::TestModule::first, 0);
    scheduler.post(test::Event::first);
    scheduler.log(core::Level::error, "startup failed");
    return core::Status::initialization_failed;
  };
  later.initializer = [&](core::InitStage) {
    ran = true;
    return core::Status::ok;
  };
  CHECK(scheduler.init() == core::Status::initialization_failed);
  CHECK(scheduler.run() == core::Status::initialization_failed);
  CHECK(scheduler.init() == core::Status::initialization_failed);
  CHECK_FALSE(ran);
#if DAVEOS_LOGGING
  REQUIRE(sink.records.size() == 1);
  CHECK(sink.records[0].message == "startup failed");
#else
  CHECK(sink.records.empty());
#endif
  CHECK(scheduler.post(test::Event::first) == core::Status::not_running);
}

TEST_CASE(
    "overdue repeats interleave by scheduled time and statistics measure "
    "duration") {
  test::Fake platform;
  test::TestModule module;
  std::vector<char> order;
  auto scheduler =
      core::make_scheduler<test::Event>(platform, core::ModuleList{&module});
  int calls = 0;
  module.first_action = [&] {
    order.push_back('a');
    platform.advance(++calls == 1 ? 25 : 1);
    if (calls == 4) scheduler.cancel(module, &test::TestModule::first);
  };
  module.second_action = [&] { order.push_back('b'); };
  module.third_action = [&] { scheduler.stop(); };
  scheduler.schedule(module, &test::TestModule::first, 10, core::Mode::repeat);
  scheduler.schedule(module, &test::TestModule::second, 25);
  scheduler.schedule(module, &test::TestModule::third, 50);
  CHECK(scheduler.run() == core::Status::ok);
  CHECK(order == std::vector<char>{'a', 'a', 'b', 'a', 'a'});
  auto stats = scheduler.snapshot();
  CHECK(stats.tasks[0].executions == 4);
  CHECK(stats.tasks[0].late_starts == 2);
  CHECK(stats.tasks[0].max_lateness == 15);
  CHECK(stats.tasks[0].min_duration == 1);
  CHECK(stats.tasks[0].max_duration == 25);
  CHECK(stats.tasks[0].average() == 7);
  scheduler.reset_statistics();
  CHECK(scheduler.snapshot().tasks[0].executions == 0);
}

TEST_CASE("replacement and self-rescheduling take precedence") {
  test::Fake platform;
  test::TestModule module;
  auto scheduler =
      core::make_scheduler<test::Event>(platform, core::ModuleList{&module});
  std::vector<core::Time> times;
  module.first_action = [&] {
    times.push_back(platform.now());
    if (times.size() == 1)
      scheduler.schedule(module, &test::TestModule::first, 7);
    else
      scheduler.stop();
  };
  CHECK(scheduler.cancel(module, &test::TestModule::first) ==
        core::Status::not_found);
  scheduler.schedule(module, &test::TestModule::first, 50);
  CHECK(scheduler.schedule(module, &test::TestModule::first, 0,
                           core::Mode::repeat) ==
        core::Status::invalid_argument);
  scheduler.schedule(module, &test::TestModule::first, 10, core::Mode::repeat);
  CHECK(scheduler.run() == core::Status::ok);
  CHECK(times == std::vector<core::Time>{10, 17});
}

TEST_CASE(
    "events preserve broadcasts, exclude sender and overflow explicitly") {
  test::Fake platform;
  test::NamedModule<"sender"> sender;
  test::NamedModule<"one"> one;
  test::NamedModule<"two"> two;
  auto scheduler = core::make_scheduler<test::Event, 1>(
      platform, core::ModuleList{&sender, &one, &two});
  std::vector<int> received;
  sender.receiver = [&](test::Event) { received.push_back(0); };
  one.receiver = [&](test::Event event) {
    received.push_back(event == test::Event::first ? 1 : 3);
    if (event == test::Event::first)
      CHECK(scheduler.post(test::Event::second, &sender) == core::Status::ok);
  };
  two.receiver = [&](test::Event event) {
    received.push_back(event == test::Event::first ? 2 : 4);
    if (event == test::Event::second) scheduler.stop();
  };
  CHECK(scheduler.post(test::Event::first, &sender) == core::Status::ok);
  CHECK(scheduler.post(test::Event::second) == core::Status::full);
  CHECK(scheduler.run() == core::Status::ok);
  REQUIRE(received.size() == 4);
  // No promise about recipient order, only complete first broadcast before
  // second.
  CHECK(received[0] + received[1] == 3);
  CHECK(received[2] + received[3] == 7);
  CHECK(scheduler.snapshot().event_overflows == 1);
}

TEST_CASE("queue wraparound, failure preservation and conveniences") {
  core::Queue<int, 2> queue;
  int out = 99;
  CHECK(queue.pop(out) == core::Status::empty);
  CHECK(out == 99);
  CHECK(queue.push(1) == core::Status::ok);
  CHECK(queue.push(2) == core::Status::ok);
  CHECK(queue.push(3) == core::Status::full);
  CHECK(queue.peek(out) == core::Status::ok);
  CHECK(out == 1);
  CHECK(queue.size() == 2);
  CHECK(queue.pop(out) == core::Status::ok);
  CHECK(queue.push(3) == core::Status::ok);
  queue.pop(out);
  CHECK(out == 2);
  queue.pop(out);
  CHECK(out == 3);
  queue.push(4);
  queue.clear();
  CHECK(queue.empty());
  CHECK(queue.capacity() == 2);
}

TEST_CASE(
    "thread-safe queue reports mutex contention without modifying output") {
  test::Fake platform;
  core::ThreadSafeQueue<int, 2, test::Fake> queue(platform);
  std::binary_semaphore locked(0), release(0);
  std::thread holder([&] {
    platform.queue_mutex()->try_lock();
    locked.release();
    release.acquire();
    platform.queue_mutex()->unlock();
  });
  locked.acquire();
  int out = 42;
  CHECK(queue.pop(out) == core::Status::busy);
  CHECK(queue.peek(out) == core::Status::busy);
  CHECK(queue.size().status == core::Status::busy);
  CHECK(out == 42);
  release.release();
  holder.join();
  CHECK(queue.push(9) == core::Status::ok);
  CHECK(queue.peek(out) == core::Status::ok);
  CHECK(out == 9);
}

TEST_CASE("interrupt scheduling during initialization uses dispatch epoch") {
  test::Fake platform;
  test::TestModule module;
  auto scheduler =
      core::make_scheduler<test::Event>(platform, core::ModuleList{&module});
  core::Time start = 0;
  module.initializer = [&](core::InitStage stage) {
    if (stage == core::InitStage::stage1) {
      platform.interrupt(
          [](void* context) {
            auto& module = *static_cast<test::TestModule*>(context);
            module.scheduler().schedule(module, &test::TestModule::first, 12);
          },
          &module);
      platform.advance(50);
    }
    return core::Status::ok;
  };
  module.first_action = [&] {
    start = platform.now();
    scheduler.stop();
  };
  CHECK(scheduler.run() == core::Status::ok);
  CHECK(start == 62);
}

TEST_CASE("CRTP modules and platforms have no virtual dispatch") {
  STATIC_REQUIRE_FALSE(std::is_polymorphic_v<test::TestModule>);
  STATIC_REQUIRE_FALSE(std::is_polymorphic_v<test::Fake>);
  STATIC_REQUIRE_FALSE(
      std::is_polymorphic_v<core::SchedulerInterface<test::Event>>);
}

TEST_CASE("thread-safe queue falls back to critical sections without a mutex") {
  struct CriticalOnly : daveos::core::Platform<CriticalOnly> {
    unsigned depth = 0;

    void enter() { ++depth; }

    void leave() { --depth; }
  } platform;

  core::ThreadSafeQueue<int, 2, CriticalOnly> queue(platform);
  CHECK(queue.push(3) == core::Status::ok);
  int value = 0;
  CHECK(queue.pop(value) == core::Status::ok);
  CHECK(value == 3);
  CHECK(platform.depth == 0);
}

namespace enum_test {
#define TEST_VALUES(X) X(negative, -4) X(zero, 0) X(next) X(sparse, 100)
DAVEOS_ENUM(Value, std::int32_t, TEST_VALUES)
#undef TEST_VALUES
static_assert(std::string_view(enum_name(Value::negative)) == "negative");
static_assert(static_cast<int>(Value::next) == 1);
static_assert(std::string_view(enum_name(static_cast<Value>(99))) == "unknown");
}  // namespace enum_test

TEST_CASE(
    "enum names preserve scoped values and handle invalid representations") {
  CHECK(std::string_view(enum_name(enum_test::Value::sparse)) == "sparse");
  CHECK(std::string_view(enum_name(core::Status::invalid_argument)) ==
        "invalid_argument");
  CHECK(std::string_view(enum_name(core::Status::too_many_arguments)) ==
        "too_many_arguments");
  CHECK(std::string_view(enum_name(static_cast<core::Status>(-1))) ==
        "unknown");
  CHECK(std::string_view(enum_name(static_cast<core::Status>(999))) ==
        "unknown");
}

TEST_CASE(
    "Fake platform reports unsupported hardware reset without changing time") {
  daveos::platform::fake::Platform platform;
  platform.advance(123);
  CHECK(platform.reset() == core::Status::unsupported);
  CHECK(platform.now() == 123);
}
