#pragma once
#include <functional>
#include <string>
#include <vector>

#include "catch_amalgamated.hpp"
#include "daveos/core/scheduler.h"
#include "daveos/platform/fake/platform.h"

namespace testing {
using namespace daveos::core;
using Fake = daveos::platform::fake::Platform;
enum class Event { first, second };
struct TestModule : daveos::core::Module<TestModule, Event> {
  explicit TestModule(const char* name = "module")
      : daveos::core::Module<TestModule, Event>(name) {}
  std::function<Status(InitStage)> initializer;
  std::function<void()> first_action, second_action, third_action;
  std::function<void(Event)> receiver;
  bool sleep = true;
  static constexpr auto tasks() {
    return std::array{TaskDescriptor<TestModule>{"first", &TestModule::first},
                      TaskDescriptor<TestModule>{"second", &TestModule::second},
                      TaskDescriptor<TestModule>{"third", &TestModule::third}};
  }
  Status init(InitStage stage) {
    return initializer ? initializer(stage) : Status::ok;
  }
  void on_event(Event event) {
    if (receiver) receiver(event);
  }
  bool can_sleep() { return sleep; }
  void first() {
    if (first_action) first_action();
  }
  void second() {
    if (second_action) second_action();
  }
  void third() {
    if (third_action) third_action();
  }
};
struct Record {
  Time timestamp;
  Level severity;
  std::string module, task, message;
};
struct Sink {
  std::vector<Record> records;
  Subscriber subscriber() {
    return {this, [](void* self, const LogRecord& record) {
              static_cast<Sink*>(self)->records.push_back(
                  {record.timestamp, record.severity, record.module,
                   record.task, std::string(record.message)});
            }};
  }
};
inline std::function<void()> timer_action, other_timer_action;
inline void Timer() {
  if (timer_action) timer_action();
}
inline void OtherTimer() {
  if (other_timer_action) other_timer_action();
}
}  // namespace testing
