#include "core/schedule/application.hpp"

#include "support.hpp"

namespace core = daveos::core;
namespace test = testing;
using std::chrono_literals::operator""ms;

namespace {
  struct Periodic : core::Module<Periodic, test::Event> {
    test::Fake& platform;
    std::array<core::Time, 3> times{};
    std::size_t calls = 0;
    bool ready = false;

    explicit Periodic(test::Fake& p) : platform(p) {}

    static constexpr const char* name() { return "periodic"; }

    static constexpr auto tasks() {
      return std::array{DAVEOS_PERIODIC(Periodic, Tick, 10ms)};
    }

    core::Status init(core::InitStage stage) {
      platform.advance(100);
      ready = stage == core::InitStage::stage2;
      return core::Status::ok;
    }

    void Tick() {
      CHECK(ready);
      times[calls++] = platform.now();
      if (calls == times.size()) {
        CHECK(scheduler().stop() == core::Status::ok);
      }
    }
  };

  // Deliberately construct Application before its module and source: only
  // addresses/static descriptors may be inspected by its constructor.
  test::Fake static_platform;
  extern Periodic static_module;
  extern core::CommandSource static_source;
  auto static_application = core::make_application<test::Event>(
      static_platform, core::ModuleList{&static_module},
      core::CommandSourceList{static_source});
  Periodic static_module{static_platform};
  core::CommandSource static_source;
}  // namespace

TEST_CASE(
    "file-scope application construction is passive and repeats from run "
    "epoch") {
  CHECK(static_module.calls == 0);
  CHECK(static_platform.now() == 0);
  CHECK(static_source.dispatch("help") == core::Status::not_running);
  CHECK(static_application.init() == core::Status::ok);
  CHECK(static_module.calls == 0);
  CHECK(static_application.init() == core::Status::already_initialized);
  static_platform.advance(1000);
  CHECK(static_application.run() == core::Status::ok);
  CHECK(static_module.times == std::array<core::Time, 3>{11200, 21200, 31200});
  CHECK(static_application.run() == core::Status::already_run);
  CHECK(static_application.initialization_failure().status == core::Status::ok);
  STATIC_REQUIRE_FALSE(
      std::is_move_constructible_v<decltype(static_application)>);
  STATIC_REQUIRE_FALSE(
      std::is_copy_constructible_v<decltype(static_application)>);
}

TEST_CASE(
    "periodic defaults precede all stage1 hooks and explicit init overrides "
    "win") {
  for (auto stage : {core::InitStage::stage1, core::InitStage::stage2}) {
    test::Fake platform;
    Periodic periodic(platform);
    test::TestModule controller;
    auto app = core::make_application<test::Event>(
        platform, core::ModuleList{&controller, &periodic});
    controller.initializer = [&](core::InitStage current) {
      if (current == stage) {
        CHECK(app.scheduler().cancel<&Periodic::Tick>(periodic) ==
              core::Status::ok);
        CHECK(app.scheduler().template schedule<&Periodic::Tick>(
                  periodic, 3ms) == core::Status::ok);
      }
      return core::Status::ok;
    };
    controller.first_action = [&] {
      CHECK(app.scheduler().stop() == core::Status::ok);
    };
    CHECK(app.scheduler().template schedule<&test::TestModule::first>(
              controller, 30ms) == core::Status::ok);
    CHECK(app.run() == core::Status::ok);
    CHECK(periodic.calls == 1);
    CHECK(periodic.times[0] == 3200);
  }
}

TEST_CASE(
    "pre-init schedule and successful cancellation override periodic "
    "defaults") {
  for (bool cancel : {false, true}) {
    test::Fake platform;
    Periodic periodic(platform);
    test::TestModule controller;
    auto app = core::make_application<test::Event>(
        platform, core::ModuleList{&periodic, &controller});
    CHECK(app.scheduler().template schedule<&Periodic::Tick>(periodic, 2ms) ==
          core::Status::ok);
    if (cancel) {
      CHECK(app.scheduler().cancel<&Periodic::Tick>(periodic) ==
            core::Status::ok);
    }
    controller.first_action = [&] {
      CHECK(app.scheduler().stop() == core::Status::ok);
    };
    CHECK(app.scheduler().template schedule<&test::TestModule::first>(
              controller, 30ms) == core::Status::ok);
    CHECK(app.run() == core::Status::ok);
    CHECK(periodic.calls == (cancel ? 0 : 1));
  }
}

TEST_CASE(
    "application failure never binds sources or dispatches periodic tasks") {
  test::Fake platform;
  Periodic periodic(platform);
  test::TestModule failed;
  core::CommandSource source;
  auto app = core::make_application<test::Event>(
      platform, core::ModuleList{&periodic, &failed},
      core::CommandSourceList{source});
  failed.initializer = [](core::InitStage) {
    return core::Status::initialization_failed;
  };
  CHECK(app.init() == core::Status::initialization_failed);
  CHECK(app.init() == core::Status::initialization_failed);
  CHECK(app.run() == core::Status::initialization_failed);
  CHECK(app.run() == core::Status::already_run);
  CHECK(source.dispatch("help") == core::Status::not_running);
  CHECK(periodic.calls == 0);
  CHECK(app.initialization_failure().status ==
        core::Status::initialization_failed);
  CHECK(std::string_view(app.initialization_failure().module) == failed.name());
}

TEST_CASE("application logging and command sources are independent") {
  struct Commands : core::Module<Commands, test::Event> {
    static constexpr const char* name() { return "commands"; }

    static constexpr auto commands() {
      return std::array{DAVEOS_COMMAND(Commands, Go, "go", "record a command")};
    }

    core::Status Go(core::CommandArguments) {
      ++calls;
      return core::Status::ok;
    }

    int calls = 0;
  };

  for (bool logging : {false, true}) {
    for (bool commands : {false, true}) {
      test::Fake platform;
      Commands receiver;
      test::TestModule controller;
      core::CommandSource source;
      unsigned logs = 0;
      auto logger = core::make_logger(
          platform, core::SubscriberList{core::Subscriber{
                        &logs, [](void* context, const core::LogRecord&) {
                          ++*static_cast<unsigned*>(context);
                        }}});
      auto modules = core::ModuleList{&receiver, &controller};
      auto sources = core::CommandSourceList{source};
      auto check = [&](auto& app) {
        controller.initializer = [&](core::InitStage) {
          CHECK(source.dispatch("commands go") == core::Status::not_running);
          return core::Status::ok;
        };
        controller.first_action = [&] {
          CHECK(source.dispatch("commands go") ==
                (commands ? core::Status::ok : core::Status::not_running));
          app.scheduler().log(core::Level::info, "hello");
          CHECK(app.scheduler().stop() == core::Status::ok);
        };
        CHECK(app.scheduler().template schedule<&test::TestModule::first>(
                  controller, 1ms) == core::Status::ok);
        CHECK(app.run() == core::Status::ok);
        CHECK(receiver.calls == (commands ? 1 : 0));
        CHECK(logs == unsigned(logging && DAVEOS_LOGGING));
      };
      if (logging && commands) {
        auto app = core::make_application<test::Event>(platform, modules,
                                                       logger, sources);
        check(app);
      } else if (logging) {
        auto app =
            core::make_application<test::Event>(platform, modules, logger);
        check(app);
      } else if (commands) {
        auto app =
            core::make_application<test::Event>(platform, modules, sources);
        check(app);
      } else {
        auto app = core::make_application<test::Event>(platform, modules);
        check(app);
      }
    }
  }
}

TEST_CASE(
    "periodic backlog keeps every iteration and init cancellation persists") {
  test::Fake platform;
  Periodic periodic(platform);
  test::TestModule controller;
  auto app = core::make_application<test::Event>(
      platform, core::ModuleList{&controller, &periodic});
  SECTION("overdue repetitions") {
    controller.first_action = [&] { platform.advance(25000); };
    CHECK(app.scheduler().schedule<&test::TestModule::first>(controller, 5ms) ==
          core::Status::ok);
    CHECK(app.run() == core::Status::ok);
    CHECK(periodic.calls == 3);
    CHECK(periodic.times == std::array<core::Time, 3>{30200, 30200, 30200});
  }
  SECTION("cancelled in stage2") {
    controller.initializer = [&](core::InitStage stage) {
      if (stage == core::InitStage::stage2) {
        return app.scheduler().cancel<&Periodic::Tick>(periodic);
      }
      return core::Status::ok;
    };
    controller.first_action = [&] {
      CHECK(app.scheduler().stop() == core::Status::ok);
    };
    CHECK(app.scheduler().schedule<&test::TestModule::first>(
              controller, 50ms) == core::Status::ok);
    CHECK(app.run() == core::Status::ok);
    CHECK(periodic.calls == 0);
  }
}

TEST_CASE(
    "NoEvent defaults and named capacities work with every service "
    "combination") {
  struct Worker : core::Module<Worker> {
    static constexpr const char* name() { return "worker"; }

    static constexpr auto tasks() {
      return std::array{DAVEOS_TASK(Worker, Tick)};
    }

    static constexpr auto commands() {
      return std::array{DAVEOS_COMMAND(Worker, Go, "go", "Go")};
    }

    core::Status Go() {
      ++calls;
      return core::Status::ok;
    }

    void Tick() {
      CHECK(source->dispatch("worker go") == core::Status::ok);
      CHECK(source->dispatch("worker go extra") ==
            core::Status::too_many_arguments);
      CHECK(source->dispatch("worker this-line-is-too-long") ==
            core::Status::line_too_long);
      scheduler().stop();
    }

    core::CommandSource* source = nullptr;
    std::uint32_t calls = 0;
  };

  STATIC_REQUIRE(std::same_as<Worker::EventType, core::NoEvent>);
  constexpr core::Capacities capacities{
      .events = 1, .timers = 2, .line = 20, .arguments = 2};
  for (bool logging : {false, true}) {
    for (bool commands : {false, true}) {
      test::Fake platform;
      Worker worker;
      core::CommandSource source;
      worker.source = &source;
      auto modules = core::ModuleList{&worker};
      auto sources = core::CommandSourceList{source};
      test::Sink sink;
      auto logger =
          core::make_logger(platform, core::SubscriberList{sink.subscriber()});
      auto check = [&](auto& app) {
        CHECK(app.scheduler().post(std::monostate{}) == core::Status::ok);
        CHECK(app.scheduler().post(std::monostate{}) == core::Status::full);
        if (commands) {
          CHECK(app.scheduler().template schedule<&Worker::Tick>(worker, 0) ==
                core::Status::ok);
          CHECK(app.run() == core::Status::ok);
          CHECK(worker.calls == 1);
        } else {
          CHECK(app.init() == core::Status::ok);
        }
      };
      if (logging && commands) {
        auto app = core::make_application<capacities>(platform, modules, logger,
                                                      sources);
        check(app);
      } else if (logging) {
        auto app =
            core::make_application<capacities>(platform, modules, logger);
        check(app);
      } else if (commands) {
        auto app =
            core::make_application<capacities>(platform, modules, sources);
        check(app);
      } else {
        auto app = core::make_application<capacities>(platform, modules);
        check(app);
      }
    }
  }
  test::Fake platform;
  Worker worker;
  auto app = core::make_application(platform, core::ModuleList{&worker});
  CHECK(app.init() == core::Status::ok);
}
