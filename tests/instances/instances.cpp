#include "core/command/command.hpp"
#include "core/schedule/application.hpp"
#include "support.hpp"

namespace core = daveos::core;
namespace test = testing;

namespace {
  // One module type that an application can register several times.
  struct Sensor : core::Module<Sensor, test::Event> {
    explicit Sensor(const char* name = nullptr) : Module(name) {}

    static constexpr const char* name() { return "sensor"; }

    static constexpr auto tasks() {
      return std::array{DAVEOS_TASK(Sensor, Poll)};
    }

    static constexpr auto commands() {
      return std::array{
          DAVEOS_COMMAND(Sensor, Sample, "sample", "Sample once")};
    }

    core::Status init(core::InitStage) {
      ++inits;
      return core::Status::ok;
    }

    void Poll() {
      ++polls;
      I_("polled");
    }

    core::Status Sample() {
      ++samples;
      I_("sampled");
      return core::Status::ok;
    }

    int inits = 0, polls = 0, samples = 0;
  };

  // Registers the application under test, runs setup before dispatch, then
  // runs action from a task scheduled 1 us later and stops. Returns the logs.
  template <typename Modules, typename Setup, typename Action>
  std::vector<test::Record> Run(Modules modules, test::TestModule& input,
                                Setup setup, Action action) {
    test::Fake platform;
    test::Sink sink;
    auto logger =
        core::make_logger(platform, core::SubscriberList{sink.subscriber()});
    auto scheduler =
        core::make_scheduler<test::Event>(platform, modules, logger);
    core::CommandDispatcher<test::Event, Modules> dispatcher{modules,
                                                             scheduler};
    REQUIRE(dispatcher.bind_sources(core::CommandSourceList{}) ==
            core::Status::ok);
    input.first_action = [&] {
      action(scheduler, dispatcher);
      scheduler.stop();
    };
    setup(scheduler);
    REQUIRE(scheduler.template schedule<&test::TestModule::first>(
                input, std::chrono::microseconds{1}) == core::Status::ok);
    REQUIRE(scheduler.run() == core::Status::ok);
    return sink.records;
  }
}  // namespace

TEST_CASE("one module type registers twice under distinct instance names") {
  Sensor left{"left"}, right{"right"};
  test::TestModule input;
  const auto records = Run(
      core::ModuleList{&left, &right, &input}, input,
      [&](auto& scheduler) {
        REQUIRE(scheduler.template schedule<&Sensor::Poll>(
                    right, std::chrono::microseconds{0}) == core::Status::ok);
      },
      [&](auto& scheduler, auto& dispatcher) {
        CHECK(dispatcher.dispatch("left sample") == core::Status::ok);
        CHECK(dispatcher.dispatch("right sample") == core::Status::ok);
        CHECK(dispatcher.dispatch("sensor sample") == core::Status::not_found);
        const auto stats = scheduler.snapshot();
        CHECK(std::string(stats.tasks[0].module) == "left");
        CHECK(std::string(stats.tasks[1].module) == "right");
      });
  CHECK(left.inits == 2);
  CHECK(right.inits == 2);
  CHECK(left.samples == 1);
  CHECK(right.samples == 1);
  CHECK(left.polls == 0);
  CHECK(right.polls == 1);
#if DAVEOS_LOGGING
  auto logged = [&](const char* module, const char* message) {
    return std::any_of(records.begin(), records.end(), [&](const auto& r) {
      return r.module == module && r.message == message;
    });
  };
  CHECK(logged("left", "sampled"));
  CHECK(logged("right", "sampled"));
  CHECK(logged("right", "polled"));
#endif
}

TEST_CASE("unnamed repeated instances fail initialization before any hook") {
  test::Fake platform;
  Sensor first, second;
  auto scheduler = core::make_scheduler<test::Event>(
      platform, core::ModuleList{&first, &second});
  CHECK(scheduler.init() == core::Status::duplicate_name);
  CHECK(first.inits == 0);
  CHECK(second.inits == 0);
  const auto failure = scheduler.initialization_failure();
  CHECK(failure.status == core::Status::duplicate_name);
  CHECK(failure.module == nullptr);
}

TEST_CASE("instance names are unique regardless of ASCII case") {
  test::Fake platform;
  Sensor first{"Probe"}, second{"probe"};
  auto scheduler = core::make_scheduler<test::Event>(
      platform, core::ModuleList{&first, &second});
  CHECK(scheduler.init() == core::Status::duplicate_name);
}

TEST_CASE("an instance name must be a valid route when it has commands") {
  test::Fake platform;
  Sensor spaced{"two words"};
  test::TestModule input;
  auto modules = core::ModuleList{&spaced, &input};
  auto scheduler = core::make_scheduler<test::Event>(platform, modules);
  core::CommandDispatcher<test::Event, decltype(modules)> dispatcher{modules,
                                                                     scheduler};
  CHECK(dispatcher.validate() == core::Status::invalid_argument);
  core::CommandSource source;
  CHECK(dispatcher.bind_sources(core::CommandSourceList{source}) ==
        core::Status::invalid_argument);
  CHECK(source.dispatch("help") == core::Status::not_running);
}

TEST_CASE("Application rejects duplicate command routes before module init") {
  // A type-level command_prefix() gives every instance the same route, even
  // though the instance names differ.
  struct Fixed : core::Module<Fixed, test::Event> {
    explicit Fixed(const char* name) : Module(name) {}

    static constexpr const char* name() { return "fixed"; }

    static constexpr const char* command_prefix() { return "shared"; }

    static constexpr auto commands() {
      return std::array{DAVEOS_COMMAND(Fixed, Go, "go", "Go")};
    }

    core::Status init(core::InitStage) {
      ++inits;
      return core::Status::ok;
    }

    core::Status Go() { return core::Status::ok; }

    int inits = 0;
  };

  test::Fake platform;
  Fixed first{"first"}, second{"second"};
  core::CommandSource source;
  auto app = core::make_application<test::Event>(
      platform, core::ModuleList{&first, &second},
      core::CommandSourceList{source});
  CHECK(app.init() == core::Status::duplicate_name);
  CHECK(app.run() == core::Status::duplicate_name);
  CHECK(first.inits == 0);
  CHECK(second.inits == 0);
  const auto failure = app.initialization_failure();
  CHECK(failure.status == core::Status::duplicate_name);
  CHECK(failure.module == nullptr);
  CHECK(source.dispatch("help") == core::Status::not_running);
}
