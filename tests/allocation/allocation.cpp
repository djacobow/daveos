#include <atomic>
#include <cstdlib>
#include <new>

#include "core/command/command.hpp"
#include "support.hpp"

namespace core = daveos::core;
namespace test = testing;

namespace {
  thread_local bool counting = false;
  std::atomic<unsigned> allocations = 0;
}  // namespace

void* operator new(std::size_t size) {
  if (counting) {
    ++allocations;
  }
  if (void* result = std::malloc(size ? size : 1)) {
    return result;
  }
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
  test::Fake platform;
  test::TestModule module;
  unsigned logs = 0;
  core::Subscriber sink{&logs, [](void* context, const core::LogRecord&) {
                          ++*static_cast<unsigned*>(context);
                        }};
  auto logger = core::make_logger(platform, core::SubscriberList{sink});
  auto scheduler = core::make_scheduler<test::Event>(
      platform, core::ModuleList{&module}, logger);
  module.first_action = [&] {
    scheduler.log(core::Level::info, "value=%d", 42);
    scheduler.post(test::Event::first);
  };
  module.receiver = [&](test::Event) { scheduler.stop(); };
  allocations = 0;
  counting = true;
  scheduler.schedule(module, &test::TestModule::first, 10);
  core::Status status = scheduler.run();
  auto statistics = scheduler.snapshot();
  scheduler.reset_statistics();
  counting = false;
  CHECK(status == core::Status::ok);
  CHECK(allocations == 0);
  CHECK(logs == unsigned(DAVEOS_LOGGING));
  CHECK(statistics.tasks[0].executions == 1);
}

TEST_CASE(
    "command parsing routing help and logging allocate no C++ heap storage") {
  struct Commands : core::Module<Commands, test::Event> {
    static constexpr const char* name() { return "commands"; }

    static constexpr auto commands() {
      return std::array{DAVEOS_COMMAND(Commands, "run", Run, "Run command")};
    }

    core::Status Run(core::CommandArguments args) {
      if (args.size() != 1 || args[0] != "one two") {
        return core::Status::invalid_argument;
      }
      I_("ran");
      return core::Status::ok;
    }
  } commands;

  test::Fake platform;
  test::TestModule input;
  auto modules = core::ModuleList{&commands, &input};
  auto logger = core::make_logger(
      platform, core::SubscriberList{core::Subscriber{
                    nullptr, [](void*, const core::LogRecord&) {}}});
  auto scheduler = core::make_scheduler<test::Event>(platform, modules, logger);
  core::CommandDispatcher dispatcher(modules, scheduler);
  core::Status command_status{}, help_status{}, invalid_status{};
  input.first_action = [&] {
    command_status = dispatcher.dispatch("commands run \"one two\"");
    help_status = dispatcher.dispatch("help");
    invalid_status = dispatcher.dispatch("unknown");
    scheduler.stop();
  };
  allocations = 0;
  counting = true;
  scheduler.schedule(input, &test::TestModule::first, 0);
  core::Status status = scheduler.run();
  counting = false;
  CHECK(status == core::Status::ok);
  CHECK(command_status == core::Status::ok);
  CHECK(help_status == core::Status::ok);
  CHECK(invalid_status == core::Status::not_found);
  CHECK(allocations == 0);
}

TEST_CASE("duration helpers and bound timers allocate no C++ heap storage") {
  struct Worker : core::Module<Worker, test::Event> {
    static constexpr const char* name() { return "worker"; }

    static constexpr auto tasks() {
      return std::array{DAVEOS_TASK(Worker, Start)};
    }

    core::Status init(core::InitStage stage) {
      return stage == core::InitStage::stage1
                 ? schedule<&Worker::Start>(std::chrono::microseconds{1})
                 : core::Status::ok;
    }

    void Start() {
      result = timer<&Worker::Done>(std::chrono::microseconds{2});
    }

    void Done() {
      result = scheduler().stop();
      ++calls;
    }

    int calls = 0;
    core::Status result = core::Status::ok;
  } worker;

  test::Fake platform;
  auto scheduler =
      core::make_scheduler<test::Event>(platform, core::ModuleList{&worker});
  allocations = 0;
  counting = true;
  const auto status = scheduler.run();
  counting = false;
  CHECK(status == core::Status::ok);
  CHECK(worker.result == core::Status::ok);
  CHECK(worker.calls == 1);
  CHECK(platform.now() == 3);
  CHECK(allocations == 0);
}
