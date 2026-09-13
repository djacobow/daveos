#include <atomic>
#include <cstdlib>
#include <new>

#include "daveos/core/command.h"
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
  auto logger = make_logger(platform, SubscriberList{sink});
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module}, logger);
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
  CHECK(logs == unsigned(DAVEOS_LOGGING));
  CHECK(statistics.tasks[0].executions == 1);
}

TEST_CASE(
    "command parsing routing help and logging allocate no C++ heap storage") {
  struct Commands : Module<Commands, Event> {
    static constexpr const char* name() { return "commands"; }
    static constexpr auto commands() {
      return std::array{DAVEOS_COMMAND(Commands, "run", Run, "Run command")};
    }
    Status Run(CommandArguments args) {
      if (args.size() != 1 || args[0] != "one two")
        return Status::invalid_argument;
      I_("ran");
      return Status::ok;
    }
  } commands;
  Fake platform;
  TestModule input;
  auto modules = ModuleList{&commands, &input};
  auto logger = make_logger(
      platform,
      SubscriberList{Subscriber{nullptr, [](void*, const LogRecord&) {}}});
  auto scheduler = make_scheduler<Event>(platform, modules, logger);
  CommandDispatcher dispatcher(modules, scheduler);
  Status command_status{}, help_status{}, invalid_status{};
  input.first_action = [&] {
    command_status = dispatcher.dispatch("commands run \"one two\"");
    help_status = dispatcher.dispatch("help");
    invalid_status = dispatcher.dispatch("unknown");
    scheduler.stop();
  };
  allocations = 0;
  counting = true;
  scheduler.schedule(input, &TestModule::first, 0);
  Status status = scheduler.run();
  counting = false;
  CHECK(status == Status::ok);
  CHECK(command_status == Status::ok);
  CHECK(help_status == Status::ok);
  CHECK(invalid_status == Status::not_found);
  CHECK(allocations == 0);
}
