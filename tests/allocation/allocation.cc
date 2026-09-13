#include <atomic>
#include <cstdlib>
#include <new>

#include "support.h"
using namespace testing;
namespace {
thread_local bool counting = false;
std::atomic<unsigned> allocations = 0;
}  // namespace
void* operator new(std::size_t size) {
  if (counting) ++allocations;
  if (void* result = std::malloc(size ? size : 1)) return result;
  std::abort();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
  std::free(pointer);
}

TEST_CASE(
    "core lifecycle, scheduling, logging and diagnostics allocate no C++ heap "
    "storage") {
  Fake platform;
  TestModule module;
  unsigned logs = 0;
  Subscriber sink{&logs, [](void* context, const LogRecord&) {
                    ++*static_cast<unsigned*>(context);
                  }};
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module},
                                         SubscriberList{sink});
  module.first_action = [&] {
    scheduler.log(Level::info, "value=%d", 42);
    scheduler.post(Event::first);
  };
  module.receiver = [&](Event) { scheduler.stop(); };
  allocations = 0;
  counting = true;
  scheduler.schedule(module, &TestModule::first, 10);
  Status status = scheduler.run();
  auto statistics = scheduler.snapshot();
  scheduler.reset_statistics();
  counting = false;
  CHECK(status == Status::ok);
  CHECK(allocations == 0);
  CHECK(logs == 1);
  CHECK(statistics.tasks[0].executions == 1);
}
