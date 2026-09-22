#include <atomic>
#include <cstdlib>
#include <new>

#include "core/command/command.hpp"
#include "core/schedule/application.hpp"
#include "hal/controller.hpp"
#include "platform/fake/bus.hpp"
#include "support.hpp"

namespace core = daveos::core;
namespace test = testing;

namespace {
#define TEST_ALLOCATION_CHOICES(X) X(on) X(off) X(toggle)
  DAVEOS_ENUM(Choice, std::uint8_t, TEST_ALLOCATION_CHOICES)
#undef TEST_ALLOCATION_CHOICES
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
    scheduler.post(test::First{});
  };
  module.receiver = [&](test::Event) { scheduler.stop(); };
  allocations = 0;
  counting = true;
  CHECK(scheduler.schedule<&test::TestModule::first>(
            module, std::chrono::microseconds{10}) == core::Status::ok);
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
      return std::array{
          DAVEOS_COMMAND(Commands, Run, "run", "Run command"),
          DAVEOS_COMMAND(Commands, Choose, "choose", "Choose mode",
                         core::arg("mode")),
          DAVEOS_COMMAND(Commands, Typed, "typed", "Typed command",
                         core::arg("rate").range(0.5f, 100.0f),
                         core::arg("count").min(1u),
                         core::arg("enabled").friendly())};
    }

    core::Status Choose(Choice value) {
      return value == Choice::toggle ? core::Status::ok
                                     : core::Status::invalid_argument;
    }

    core::Status Typed(float rate, std::uint32_t count,
                       std::optional<bool> enabled) {
      return rate == 1.25f && count == 16 && enabled == true
                 ? core::Status::ok
                 : core::Status::invalid_argument;
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
  core::Status command_status{}, help_status{}, invalid_status{},
      typed_status{}, choice_status{};
  input.first_action = [&] {
    command_status = dispatcher.dispatch("commands run \"one two\"");
    typed_status = dispatcher.dispatch("commands typed 1.25 0x10 high");
    choice_status = dispatcher.dispatch("commands choose tog");
    help_status = dispatcher.dispatch("help");
    invalid_status = dispatcher.dispatch("unknown");
    scheduler.stop();
  };
  allocations = 0;
  counting = true;
  CHECK(scheduler.schedule<&test::TestModule::first>(
            input, std::chrono::microseconds{0}) == core::Status::ok);
  core::Status status = scheduler.run();
  counting = false;
  CHECK(status == core::Status::ok);
  CHECK(command_status == core::Status::ok);
  CHECK(typed_status == core::Status::ok);
  CHECK(choice_status == core::Status::ok);
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

TEST_CASE(
    "application construction and declarative dispatch allocate no C++ heap") {
  struct Worker : core::Module<Worker, test::Event> {
    core::CommandSource& source;
    int calls = 0;
    core::Status dispatched = core::Status::not_running;
    core::Status stopped = core::Status::not_running;

    explicit Worker(core::CommandSource& s) : source(s) {}

    static constexpr const char* name() { return "worker"; }

    static constexpr auto tasks() {
      return std::array{
          DAVEOS_PERIODIC(Worker, Poll, std::chrono::milliseconds{1})};
    }

    static constexpr auto commands() {
      return std::array{DAVEOS_COMMAND(Worker, Go, "go", "record invocation")};
    }

    core::Status Go(core::CommandArguments) {
      ++calls;
      return core::Status::ok;
    }

    void Poll() {
      dispatched = source.dispatch("worker go");
      scheduler().log(core::Level::info, "done");
      stopped = scheduler().stop();
    }
  };

  test::Fake platform;
  core::CommandSource source;
  Worker worker(source);
  unsigned logs = 0;
  auto logger = core::make_logger(
      platform, core::SubscriberList{core::Subscriber{
                    &logs, [](void* p, const core::LogRecord&) {
                      ++*static_cast<unsigned*>(p);
                    }}});
  allocations = 0;
  counting = true;
  core::Status status;
  {
    auto app = core::make_application<test::Event>(
        platform, core::ModuleList{&worker}, logger,
        core::CommandSourceList{source});
    status = app.run();
  }
  counting = false;
  CHECK(allocations == 0);
  CHECK(status == core::Status::ok);
  CHECK(worker.dispatched == core::Status::ok);
  CHECK(worker.stopped == core::Status::ok);
  CHECK(worker.calls == 1);
  CHECK(logs == unsigned(DAVEOS_LOGGING));
}

TEST_CASE(
    "typed payload delivery and handler logging allocate no heap storage") {
  struct Payload {
    std::array<std::uint32_t, 8> data{};
  };

  using Event = std::variant<Payload>;

  struct Receiver : core::Module<Receiver, Event> {
    static constexpr const char* name() { return "receiver"; }

    static constexpr auto events() {
      return std::tuple{DAVEOS_EVENT(Receiver, Receive)};
    }

    void Receive(const Payload& payload) {
      received = payload.data[7];
      I_("received payload");
      scheduler().stop();
    }

    std::uint32_t received = 0;
  } module;

  test::Fake platform;
  auto logger = core::make_logger(
      platform, core::SubscriberList{core::Subscriber{
                    nullptr, [](void*, const core::LogRecord&) {}}});
  auto scheduler =
      core::make_scheduler<Event>(platform, core::ModuleList{&module}, logger);
  Payload payload;
  payload.data[7] = 42;
  allocations = 0;
  counting = true;
  const auto posted = scheduler.post(payload);
  const auto ran = scheduler.run();
  counting = false;
  CHECK(posted == core::Status::ok);
  CHECK(ran == core::Status::ok);
  CHECK(module.received == 42);
  CHECK(allocations == 0);
}

TEST_CASE(
    "Bus submission, interrupt completion and statistics allocate no heap "
    "storage") {
  namespace hal = daveos::hal;
  namespace fake = daveos::platform::fake;
  fake::BusClock<> clock;
  fake::BusCritical critical;
  fake::SpiBus backend;
  hal::Controller<fake::SpiBus, decltype(clock), fake::BusCritical, 1> bus{
      backend, clock, critical, {{{1, 100000}}}};
  REQUIRE(bus.init() == hal::Status::ok);
  const std::array actions{hal::spi::idle_clocks(80)};
  const std::array script{fake::SpiBus::Step{actions[0]}};
  backend.script(script);
  hal::spi::Completion completion;
  allocations = 0;
  counting = true;
  const auto status = completion.start(bus.device<0>(), actions);
  while (backend.take_pending()) {
    bus.interrupt();
  }
  const auto stats = bus.statistics();
  const auto result = completion.result();
  counting = false;
  REQUIRE(allocations == 0);
  REQUIRE(status == hal::Status::ok);
  REQUIRE(result->status == hal::Status::ok);
  REQUIRE(stats.completed == 1);
}

TEST_CASE("nested task yielding allocates no heap storage") {
  struct Module : core::Module<Module> {
    static constexpr const char* name() { return "yield"; }

    static constexpr auto tasks() {
      return std::array{DAVEOS_TASK(Module, Outer), DAVEOS_TASK(Module, Inner)};
    }

    core::Status yielded = core::Status::empty;
    std::uint32_t calls = 0;

    void Outer() {
      yielded = scheduler().yield();
      scheduler().stop();
    }

    void Inner() { ++calls; }
  } module;

  test::Fake platform;
  auto scheduler = core::make_scheduler(platform, core::ModuleList{&module});
  CHECK(scheduler.schedule<&Module::Outer>(
            module, std::chrono::microseconds{0}) == core::Status::ok);
  CHECK(scheduler.schedule<&Module::Inner>(
            module, std::chrono::microseconds{0}) == core::Status::ok);
  allocations = 0;
  counting = true;
  const auto status = scheduler.run();
  counting = false;
  CHECK(status == core::Status::ok);
  CHECK(module.yielded == core::Status::ok);
  CHECK(module.calls == 1);
  CHECK(allocations == 0);
}
