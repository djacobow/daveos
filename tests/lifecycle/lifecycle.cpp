// Partial initialization and explicit cleanup. DaveOS has no Module::stop()
// hook: after a failed init the application stops its own components, in
// reverse order, including ones that never initialized, and lets in-flight
// hardware work finish or time out before tearing down the bus.
#include <string>
#include <vector>

#include "console/transport.hpp"
#include "core/schedule/application.hpp"
#include "hal/controller.hpp"
#include "platform/fake/bus.hpp"
#include "platform/stm32/console/console.hpp"
#include "support.hpp"

namespace core = daveos::core;
namespace hal = daveos::hal;
namespace spi = hal::spi;
namespace fake = daveos::platform::fake;
namespace test = testing;

namespace {
  using Stage = core::InitStage;

  // One type, several named instances; records each init stage it sees.
  struct Step : core::Module<Step, test::Event> {
    Step(const char* name, std::vector<std::string>& log,
         std::optional<Stage> fail = std::nullopt)
        : Module(name), log(log), fail(fail) {}

    static constexpr const char* name() { return "step"; }

    core::Status init(Stage stage) {
      log.push_back(std::string(module_name()) +
                    (stage == Stage::stage1 ? ":1" : ":2"));
      return stage == fail ? core::Status::initialization_failed
                           : core::Status::ok;
    }

    std::vector<std::string>& log;
    std::optional<Stage> fail;
  };

  // Starts a bus transfer during stage1 that is still in flight when a later
  // module fails. Its buffer stays borrowed until the HAL completes it.
  struct Sensor : core::Module<Sensor, test::Event> {
    explicit Sensor(const spi::Device& device) : device(device) {}

    static constexpr const char* name() { return "sensor"; }

    core::Status init(Stage stage) {
      if (stage == Stage::stage1) {
        return completion.start(device, actions,
                                std::chrono::milliseconds{10}) ==
                       hal::Status::ok
                   ? core::Status::ok
                   : core::Status::initialization_failed;
      }
      return core::Status::ok;
    }

    spi::Device device;
    std::array<std::uint8_t, 2> command{0x9f, 0};
    std::array<spi::Action, 1> actions{spi::write(command)};
    spi::Completion completion;
  };

  struct TransportState {
    bool initialized = false;
    std::vector<int>& stops;
    int id;
  };

  struct Transport {
    TransportState& state;

    explicit Transport(TransportState& value) : state(value) {}

    static constexpr const char* name() { return "serial"; }

    static constexpr const char* statistics_label() { return "serial"; }

    bool init() {
      state.initialized = true;
      return true;
    }

    // Must be safe whether or not init() ran.
    void stop() { state.stops.push_back(state.id); }

    bool poll_line(daveos::console::Line&) { return false; }

    std::uint32_t take_dropped() { return 0; }

    void output(const core::LogRecord&) {}

    daveos::console::TxCounters counters() { return {}; }
  };

  using Console = daveos::console::TransportModule<test::Event, Transport>;

  struct Bus {
    test::Fake platform;
    fake::BusClock<> clock;
    fake::BusCritical critical;
    fake::SpiBus backend;
    hal::Controller<fake::SpiBus, fake::BusClock<>, fake::BusCritical, 1> bus{
        backend, clock, critical, {{{1, 1000000}}}};

    void Advance(std::uint64_t us) {
      clock.advance(us);
      while (auto callback = clock.take_due()) {
        REQUIRE(platform.interrupt(
                    [](void* p) { (*static_cast<core::TimerCallback*>(p))(); },
                    &callback) == core::Status::ok);
      }
    }
  };
}  // namespace

TEST_CASE(
    "stage1 failure part-way leaves later modules untouched and explicit "
    "cleanup stops everything") {
  Bus hardware;
  REQUIRE(hardware.bus.init() == hal::Status::ok);
  // The transfer's single step never completes on its own.
  std::array<std::uint8_t, 2> expected{0x9f, 0};
  const std::array script{
      fake::SpiBus::Step{spi::write(expected), {}, hal::Status::ok, true}};
  hardware.backend.script(script);

  std::vector<std::string> log;
  std::vector<int> stops;
  TransportState uart_state{false, stops, 1}, usb_state{false, stops, 2};
  Step first{"first", log};
  Sensor sensor{hardware.bus.device<0>()};
  Step failing{"failing", log, Stage::stage1};
  Step later{"later", log};
  Console uart{uart_state, "uart"}, usb{usb_state, "usb"};
  daveos::platform::stm32::Console console{uart, usb};
  test::Sink sink;
  auto logger = core::make_logger(hardware.platform,
                                  core::SubscriberList{sink.subscriber()});
  auto app = core::make_application<test::Event>(
      hardware.platform, console.modules(first, sensor, failing, later), logger,
      console.sources());

  CHECK(app.run() == core::Status::initialization_failed);
  const auto failure = app.initialization_failure();
  CHECK(failure.status == core::Status::initialization_failed);
  CHECK(std::string(failure.module) == "failing");
  CHECK(failure.stage == Stage::stage1);
  // Modules after the failure never initialize; nothing reaches stage2.
  CHECK(log == std::vector<std::string>{"first:1", "failing:1"});
  CHECK_FALSE(uart_state.initialized);
  CHECK_FALSE(usb_state.initialized);
  // Sources bind only after a successful init.
  CHECK(uart.command_source().dispatch("help") == core::Status::not_running);
  // The failure did not cancel the sensor's transfer: CS is still asserted
  // and its buffer is still borrowed.
  CHECK_FALSE(sensor.completion.ready());
  CHECK(hardware.backend.selected);
#if DAVEOS_LOGGING
  CHECK(std::any_of(sink.records.begin(), sink.records.end(), [](auto& r) {
    return r.message ==
           "Initialization failed: initialization_failed (failing, stage 1)";
  }));
#endif

  // Application cleanup: transports in reverse registration order, including
  // ones that never initialized...
  console.stop();
  CHECK(stops == std::vector<int>{2, 1});
  // ...then in-flight bus work must finish or time out before the controller
  // is torn down.
  hardware.Advance(10000);
  REQUIRE(sensor.completion.ready());
  CHECK(sensor.completion.result()->status == hal::Status::timeout);
  CHECK_FALSE(hardware.backend.selected);
  hardware.bus.deinit();
}

TEST_CASE(
    "stage2 failure follows completed stage1 and still stops initialized "
    "components") {
  test::Fake platform;
  std::vector<std::string> log;
  std::vector<int> stops;
  TransportState uart_state{false, stops, 1};
  Step a{"a", log}, b{"b", log, Stage::stage2}, c{"c", log};
  Console uart{uart_state, "uart"};
  daveos::platform::stm32::Console console{uart};
  auto app = core::make_application<test::Event>(
      platform, console.modules(a, b, c), console.sources());

  CHECK(app.run() == core::Status::initialization_failed);
  const auto failure = app.initialization_failure();
  CHECK(std::string(failure.module) == "b");
  CHECK(failure.stage == Stage::stage2);
  // Every stage1 completes before any stage2; stage2 stops at the failure.
  CHECK(log == std::vector<std::string>{"a:1", "b:1", "c:1", "a:2", "b:2"});
  CHECK(uart_state.initialized);
  CHECK(uart.command_source().dispatch("help") == core::Status::not_running);
  console.stop();
  CHECK(stops == std::vector<int>{1});
}
