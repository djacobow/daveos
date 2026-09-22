#include "core/command/command.hpp"
#include "core/logging/logger.hpp"
#include "core/schedule/application.hpp"
#include "core/schedule/scheduler.hpp"

namespace core = daveos::core;

namespace {

  struct SampleEvent {
    std::uint32_t value = 0;
  };

  using Event = std::variant<SampleEvent>;
#define ARM_MODES(X) X(start, -1) X(stop, 7)
  DAVEOS_ENUM(Mode, std::int8_t, ARM_MODES)
#undef ARM_MODES
  inline constexpr std::array modes{core::EnumChoice{"start", Mode::start},
                                    core::EnumChoice{"stop", Mode::stop}};

  struct Example : core::Module<Example, Event> {
    static constexpr const char* name() { return "arm_compile"; }

    static constexpr auto tasks() {
      return std::array{
          DAVEOS_PERIODIC(Example, tick, std::chrono::milliseconds{1})};
    }

    static constexpr auto commands() {
      return std::array{DAVEOS_COMMAND(Example, Run, "run", "Schedule tick"),
                        DAVEOS_COMMAND(Example, Sample, "sample", "Sample",
                                       core::arg("rate").range(0.5f, 100.0f),
                                       core::arg("enabled").friendly(),
                                       core::arg("mode").choices<modes>())};
    }

    core::Status Run() {
      return scheduler().schedule(*this, &Example::tick, 0);
    }

    core::Status Sample(float, std::optional<bool>, std::optional<Mode>) {
      return core::Status::ok;
    }

    static constexpr auto events() {
      return std::tuple{DAVEOS_EVENT(Example, OnSample)};
    }

    void OnSample(const SampleEvent&) {}

    void tick() {
      scheduler().post(SampleEvent{42}, this);
      scheduler().stop();
    }
  };

  static_assert(std::string_view(core::command<&Example::Sample>(
                                     "sample", "Sample", core::arg("rate"),
                                     core::arg("enabled"), core::arg("mode"))
                                     .handler) == "sample");
}  // namespace

// Declaration-only platform: exercise core templates without any host/OS
// dependencies.
struct CompilePlatform : daveos::core::Platform<CompilePlatform> {
  daveos::core::Time now() const;
  void enter();
  void leave();
  void arm(daveos::core::Time, Callback, void*);
  void disarm();
  void quiesce();
  bool in_interrupt() const;
  daveos::core::Context context() const;
  void context(daveos::core::Context);
  std::uint64_t sequence() const;
  void notify();
  void idle(daveos::core::Time, bool, std::uint64_t);
};

void Instantiate(CompilePlatform& platform) {
  Example module;
  auto scheduler = daveos::core::make_scheduler<Event>(
      platform, daveos::core::ModuleList{&module});
  core::CommandDispatcher dispatcher(core::ModuleList{&module}, scheduler);
  dispatcher.dispatch("arm_compile run");
  (void)scheduler.run();
  scheduler.snapshot();
  daveos::core::ThreadSafeQueue<int, 8, CompilePlatform> queue(platform);
  queue.push(1);
}

// Also instantiate the optional service and formatting path for the MCU ABI.
void InstantiateLogging(CompilePlatform& platform) {
  Example module;
  auto logger = core::make_logger(
      platform, core::SubscriberList{core::Subscriber{
                    nullptr, [](void*, const core::LogRecord&) {}}});
  auto scheduler =
      core::make_scheduler<Event>(platform, core::ModuleList{&module}, logger);
  scheduler.log(core::Level::info, "ARM log %d", 1);
  scheduler.log_statistics();
  logger.minimum(core::Level::debug);
  logger.counters();
  logger.reset();
  (void)scheduler.run();
}

void InstantiateApplication(CompilePlatform& platform) {
  Example module;
  core::CommandSource source;
  auto logger = core::make_logger(platform, core::SubscriberList{});
  auto app = core::make_application<core::Capacities{.events = 8}, Event>(
      platform, core::ModuleList{&module}, logger,
      core::CommandSourceList{source});
  (void)app.run();
  (void)app.initialization_failure();
}
