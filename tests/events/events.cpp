#include <thread>

#include "support.hpp"

namespace core = daveos::core;
namespace test = testing;

namespace {
  struct Reading {
    std::uint32_t value = 0;
    std::array<char, 8> label{};
  };

  struct Done {};

  struct Ignored {};

  using Event = std::variant<Reading, Done, Ignored>;

  struct Receiver : core::Module<Receiver, Event> {
    static constexpr const char* name() { return "receiver"; }

    static constexpr auto events() {
      return std::tuple{DAVEOS_EVENT(Receiver, OnReading),
                        DAVEOS_EVENT(Receiver, OnDone)};
    }

    void OnReading(const Reading& reading) {
      values.push_back(reading);
      I_("reading");
    }

    static constexpr auto tasks() {
      return std::array{DAVEOS_TASK(Receiver, Finish)};
    }

    void Finish() { CHECK(scheduler().post(Done{}) == core::Status::ok); }

    void OnDone(const Done&) { CHECK(scheduler().stop() == core::Status::ok); }

    std::vector<Reading> values;
  };

  struct Visitor : core::Module<Visitor, Event> {
    static constexpr const char* name() { return "visitor"; }

    void on_event(const Event& event) {
      std::visit(
          [&](const auto& payload) {
            using P = std::remove_cvref_t<decltype(payload)>;
            if constexpr (std::same_as<P, Reading>) {
              values.push_back(payload.value);
            } else if constexpr (std::same_as<P, Done>) {
              ++done;
            }
          },
          event);
    }

    std::vector<std::uint32_t> values;
    std::uint32_t done = 0;
  };

  struct Empty : core::Module<Empty, Event> {
    static constexpr const char* name() { return "empty"; }
  };
}  // namespace

TEST_CASE("variant events own queued values and attribute named handlers") {
  test::Fake platform;
  Receiver receiver;
  Visitor visitor;
  Empty empty;
  test::Sink sink;
  auto logger =
      core::make_logger(platform, core::SubscriberList{sink.subscriber()});
  auto scheduler = core::make_scheduler<Event, 4>(
      platform, core::ModuleList{&receiver, &visitor, &empty}, logger);
  Reading original{42, {'o', 'k'}};
  REQUIRE(scheduler.post(original) == core::Status::ok);
  original.value = 99;
  original.label[0] = 'x';
  REQUIRE(scheduler.post(Event{Reading{7}}, &visitor) == core::Status::ok);
  REQUIRE(scheduler.post(Ignored{}) == core::Status::ok);
  REQUIRE(scheduler.post(Ignored{}) == core::Status::ok);
  REQUIRE(scheduler.post(Reading{100}) == core::Status::full);
  REQUIRE(scheduler.schedule<&Receiver::Finish>(
              receiver, std::chrono::microseconds{1}) == core::Status::ok);
  REQUIRE(scheduler.run() == core::Status::ok);
  std::sort(
      receiver.values.begin(), receiver.values.end(),
      [](const Reading& a, const Reading& b) { return a.value > b.value; });
  REQUIRE(receiver.values.size() == 2);
  CHECK(receiver.values[0].value == 42);
  CHECK(receiver.values[0].label[0] == 'o');
  CHECK(receiver.values[1].value == 7);
  CHECK(visitor.values == std::vector<std::uint32_t>{42});
  CHECK(visitor.done == 1);  // Stop does not interrupt the current broadcast.
  CHECK(scheduler.snapshot().event_overflows == 1);
#if DAVEOS_LOGGING
  REQUIRE(sink.records.size() == 2);
  for (const auto& record : sink.records) {
    CHECK(record.module == "receiver");
    CHECK(record.task == "OnReading");
  }
#else
  CHECK(sink.records.empty());
#endif
}

TEST_CASE(
    "interrupt posts copy payloads and dispatch outside interrupt context") {
  struct Single : core::Module<Single, Event> {
    static constexpr const char* name() { return "single"; }

    static constexpr auto events() {
      return std::tuple{DAVEOS_EVENT(Single, Read)};
    }

    void Read(const Reading& value) noexcept {
      CHECK_FALSE(platform->in_interrupt());
      CHECK(value.value == 123);
      CHECK(scheduler().stop() == core::Status::ok);
    }

    test::Fake* platform = nullptr;
  } module;

  test::Fake platform;
  module.platform = &platform;
  auto scheduler =
      core::make_scheduler<Event>(platform, core::ModuleList{&module});
  using Scheduler = decltype(scheduler);
  // Interrupt posts during initialization retain the same copy semantics.
  REQUIRE(platform.interrupt(
              [](void* context) {
                auto& scheduler = *static_cast<Scheduler*>(context);
                Reading value{123};
                CHECK(scheduler.post(value) == core::Status::ok);
                value.value = 456;
              },
              &scheduler) == core::Status::ok);
  REQUIRE(scheduler.run() == core::Status::ok);
}

TEST_CASE(
    "payload posting from a concurrent interrupt preserves handler context") {
  struct Concurrent : core::Module<Concurrent, Event> {
    static constexpr const char* name() { return "concurrent"; }

    static constexpr auto tasks() {
      return std::array{DAVEOS_TASK(Concurrent, Start)};
    }

    static constexpr auto events() {
      return std::tuple{DAVEOS_EVENT(Concurrent, Read)};
    }

    void Start() {
      std::thread interrupt([&] {
        interrupt_status = platform->interrupt(
            [](void* context) {
              auto& self = *static_cast<Concurrent*>(context);
              Reading payload{321};
              self.post_status = self.scheduler().post(payload);
              payload.value = 0;
            },
            this);
      });
      interrupt.join();
      I_("task context");
    }

    void Read(const Reading& payload) {
      value = payload.value;
      I_("event context");
      scheduler().post(Ignored{});
      scheduler().stop();
    }

    test::Fake* platform = nullptr;
    core::Status interrupt_status = core::Status::not_running;
    core::Status post_status = core::Status::not_running;
    std::uint32_t value = 0;
  } module;

  test::Fake platform;
  module.platform = &platform;
  test::Sink sink;
  auto logger =
      core::make_logger(platform, core::SubscriberList{sink.subscriber()});
  auto scheduler =
      core::make_scheduler<Event>(platform, core::ModuleList{&module}, logger);
  REQUIRE(scheduler.schedule<&Concurrent::Start>(
              module, std::chrono::microseconds{0}) == core::Status::ok);
  REQUIRE(scheduler.run() == core::Status::ok);
  CHECK(module.interrupt_status == core::Status::ok);
  CHECK(module.post_status == core::Status::ok);
  CHECK(module.value == 321);
#if DAVEOS_LOGGING
  REQUIRE(sink.records.size() == 2);
  CHECK(sink.records[0].task == "Start");
  CHECK(sink.records[1].task == "Read");
#endif
}
