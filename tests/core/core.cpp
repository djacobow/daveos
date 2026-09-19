#include <memory>
#include <semaphore>
#include <thread>

#include "support.hpp"
using namespace testing;

TEST_CASE("scheduler construction does not require constructed modules") {
  Fake platform;
  alignas(TestModule) std::byte storage[sizeof(TestModule)];
  auto* module = reinterpret_cast<TestModule*>(storage);
  // The scheduler may retain addresses and static descriptors, but must not
  // touch the module until init. Its constructor runs later, as across TUs.
  auto scheduler = make_scheduler<Event>(platform, ModuleList{module});
  auto destroy = [](TestModule* p) { std::destroy_at(p); };
  std::unique_ptr<TestModule, decltype(destroy)> owned(
      std::construct_at(module), destroy);
  std::vector<InitStage> stages;
  owned->initializer = [&](InitStage stage) {
    CHECK(&owned->scheduler() == &scheduler);
    stages.push_back(stage);
    return Status::ok;
  };
  CHECK(scheduler.init() == Status::ok);
  CHECK(stages == std::vector{InitStage::stage1, InitStage::stage2});
}

TEST_CASE("startup is two passes; task deadlines start at run") {
  Fake platform;
  NamedModule<"one"> one;
  NamedModule<"two"> two;
  std::vector<int> stages;
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&one, &two});
  one.initializer = [&](InitStage stage) {
    stages.push_back(stage == InitStage::stage1 ? 1 : 3);
    if (stage == InitStage::stage1) {
      CHECK(&one.scheduler() == &scheduler);
      CHECK(scheduler.schedule(one, &decltype(one)::first, 10) == Status::ok);
      platform.advance(100);
    }
    return Status::ok;
  };
  two.initializer = [&](InitStage stage) {
    stages.push_back(stage == InitStage::stage1 ? 2 : 4);
    return Status::ok;
  };
  Time fired = 0;
  one.first_action = [&] {
    fired = platform.now();
    scheduler.stop();
  };
  CHECK(scheduler.stop() == Status::not_running);
  CHECK(scheduler.init() == Status::ok);
  CHECK(stages == std::vector<int>{1, 2, 3, 4});
  CHECK(scheduler.init() == Status::already_initialized);
  platform.advance(500);
  CHECK(scheduler.run() == Status::ok);
  CHECK(fired == 610);
  CHECK(scheduler.snapshot().tasks[0].late_starts == 0);
  CHECK(scheduler.run() == Status::already_run);
  CHECK(scheduler.stop() == Status::not_running);
}

TEST_CASE("initialization failure discards work and flushes diagnostics") {
  Fake platform;
  TestModule module;
  NamedModule<"later"> later;
  Sink sink;
  bool ran = false;
  auto logger = make_logger(platform, SubscriberList{sink.subscriber()});
  auto scheduler =
      make_scheduler<Event>(platform, ModuleList{&module, &later}, logger);
  module.first_action = [&] { ran = true; };
  module.initializer = [&](InitStage) {
    scheduler.schedule(module, &TestModule::first, 0);
    scheduler.post(Event::first);
    scheduler.log(Level::error, "startup failed");
    return Status::initialization_failed;
  };
  later.initializer = [&](InitStage) {
    ran = true;
    return Status::ok;
  };
  CHECK(scheduler.init() == Status::initialization_failed);
  CHECK(scheduler.run() == Status::initialization_failed);
  CHECK(scheduler.init() == Status::initialization_failed);
  CHECK_FALSE(ran);
#if DAVEOS_LOGGING
  REQUIRE(sink.records.size() == 1);
  CHECK(sink.records[0].message == "startup failed");
#else
  CHECK(sink.records.empty());
#endif
  CHECK(scheduler.post(Event::first) == Status::not_running);
}

TEST_CASE(
    "overdue repeats interleave by scheduled time and statistics measure "
    "duration") {
  Fake platform;
  TestModule module;
  std::vector<char> order;
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module});
  int calls = 0;
  module.first_action = [&] {
    order.push_back('a');
    platform.advance(++calls == 1 ? 25 : 1);
    if (calls == 4) scheduler.cancel(module, &TestModule::first);
  };
  module.second_action = [&] { order.push_back('b'); };
  module.third_action = [&] { scheduler.stop(); };
  scheduler.schedule(module, &TestModule::first, 10, Mode::repeat);
  scheduler.schedule(module, &TestModule::second, 25);
  scheduler.schedule(module, &TestModule::third, 50);
  CHECK(scheduler.run() == Status::ok);
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
  Fake platform;
  TestModule module;
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module});
  std::vector<Time> times;
  module.first_action = [&] {
    times.push_back(platform.now());
    if (times.size() == 1)
      scheduler.schedule(module, &TestModule::first, 7);
    else
      scheduler.stop();
  };
  CHECK(scheduler.cancel(module, &TestModule::first) == Status::not_found);
  scheduler.schedule(module, &TestModule::first, 50);
  CHECK(scheduler.schedule(module, &TestModule::first, 0, Mode::repeat) ==
        Status::invalid_argument);
  scheduler.schedule(module, &TestModule::first, 10, Mode::repeat);
  CHECK(scheduler.run() == Status::ok);
  CHECK(times == std::vector<Time>{10, 17});
}

TEST_CASE(
    "events preserve broadcasts, exclude sender and overflow explicitly") {
  Fake platform;
  NamedModule<"sender"> sender;
  NamedModule<"one"> one;
  NamedModule<"two"> two;
  auto scheduler =
      make_scheduler<Event, 1>(platform, ModuleList{&sender, &one, &two});
  std::vector<int> received;
  sender.receiver = [&](Event) { received.push_back(0); };
  one.receiver = [&](Event event) {
    received.push_back(event == Event::first ? 1 : 3);
    if (event == Event::first)
      CHECK(scheduler.post(Event::second, &sender) == Status::ok);
  };
  two.receiver = [&](Event event) {
    received.push_back(event == Event::first ? 2 : 4);
    if (event == Event::second) scheduler.stop();
  };
  CHECK(scheduler.post(Event::first, &sender) == Status::ok);
  CHECK(scheduler.post(Event::second) == Status::full);
  CHECK(scheduler.run() == Status::ok);
  REQUIRE(received.size() == 4);
  // No promise about recipient order, only complete first broadcast before
  // second.
  CHECK(received[0] + received[1] == 3);
  CHECK(received[2] + received[3] == 7);
  CHECK(scheduler.snapshot().event_overflows == 1);
}

TEST_CASE("queue wraparound, failure preservation and conveniences") {
  Queue<int, 2> queue;
  int out = 99;
  CHECK(queue.pop(out) == Status::empty);
  CHECK(out == 99);
  CHECK(queue.push(1) == Status::ok);
  CHECK(queue.push(2) == Status::ok);
  CHECK(queue.push(3) == Status::full);
  CHECK(queue.peek(out) == Status::ok);
  CHECK(out == 1);
  CHECK(queue.size() == 2);
  CHECK(queue.pop(out) == Status::ok);
  CHECK(queue.push(3) == Status::ok);
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
  Fake platform;
  ThreadSafeQueue<int, 2, Fake> queue(platform);
  std::binary_semaphore locked(0), release(0);
  std::thread holder([&] {
    platform.queue_mutex()->try_lock();
    locked.release();
    release.acquire();
    platform.queue_mutex()->unlock();
  });
  locked.acquire();
  int out = 42;
  CHECK(queue.pop(out) == Status::busy);
  CHECK(queue.peek(out) == Status::busy);
  CHECK(queue.size().status == Status::busy);
  CHECK(out == 42);
  release.release();
  holder.join();
  CHECK(queue.push(9) == Status::ok);
  CHECK(queue.peek(out) == Status::ok);
  CHECK(out == 9);
}

TEST_CASE("interrupt scheduling during initialization uses dispatch epoch") {
  Fake platform;
  TestModule module;
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module});
  Time start = 0;
  module.initializer = [&](InitStage stage) {
    if (stage == InitStage::stage1) {
      platform.interrupt(
          [](void* context) {
            auto& module = *static_cast<TestModule*>(context);
            module.scheduler().schedule(module, &TestModule::first, 12);
          },
          &module);
      platform.advance(50);
    }
    return Status::ok;
  };
  module.first_action = [&] {
    start = platform.now();
    scheduler.stop();
  };
  CHECK(scheduler.run() == Status::ok);
  CHECK(start == 62);
}

TEST_CASE("CRTP modules and platforms have no virtual dispatch") {
  STATIC_REQUIRE_FALSE(std::is_polymorphic_v<TestModule>);
  STATIC_REQUIRE_FALSE(std::is_polymorphic_v<Fake>);
  STATIC_REQUIRE_FALSE(std::is_polymorphic_v<SchedulerInterface<Event>>);
}

TEST_CASE("thread-safe queue falls back to critical sections without a mutex") {
  struct CriticalOnly : daveos::core::Platform<CriticalOnly> {
    unsigned depth = 0;
    void enter() { ++depth; }
    void leave() { --depth; }
  } platform;
  ThreadSafeQueue<int, 2, CriticalOnly> queue(platform);
  CHECK(queue.push(3) == Status::ok);
  int value = 0;
  CHECK(queue.pop(value) == Status::ok);
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
  CHECK(std::string_view(enum_name(Status::invalid_argument)) ==
        "invalid_argument");
  CHECK(std::string_view(enum_name(Status::too_many_arguments)) ==
        "too_many_arguments");
  CHECK(std::string_view(enum_name(static_cast<Status>(-1))) == "unknown");
  CHECK(std::string_view(enum_name(static_cast<Status>(999))) == "unknown");
}

TEST_CASE(
    "Fake platform reports unsupported hardware reset without changing time") {
  daveos::platform::fake::Platform platform;
  platform.advance(123);
  CHECK(platform.reset() == Status::unsupported);
  CHECK(platform.now() == 123);
}
