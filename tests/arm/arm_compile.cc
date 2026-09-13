#include "daveos/core/command.h"
#include "daveos/core/logger.h"
#include "daveos/core/scheduler.h"
namespace {
using namespace daveos::core;
enum class Event { sample };
struct Example : Module<Example, Event> {
  static constexpr const char* name() { return "arm_compile"; }
  static constexpr auto tasks() {
    return std::array{TaskDescriptor<Example>{"tick", &Example::tick}};
  }
  static constexpr auto commands() {
    return std::array{DAVEOS_COMMAND(Example, "run", Run, "Schedule tick")};
  }
  Status Run(CommandArguments args) {
    if (!args.empty()) return Status::invalid_argument;
    return scheduler().schedule(*this, &Example::tick, 0);
  }
  void tick() {
    scheduler().post(Event::sample, this);
    scheduler().stop();
  }
};
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
  CommandDispatcher dispatcher(ModuleList{&module}, scheduler);
  dispatcher.dispatch("arm_compile run");
  scheduler.run();
  scheduler.snapshot();
  daveos::core::ThreadSafeQueue<int, 8, CompilePlatform> queue(platform);
  queue.push(1);
}

// Also instantiate the optional service and formatting path for the MCU ABI.
void InstantiateLogging(CompilePlatform& platform) {
  Example module;
  auto logger = make_logger(
      platform,
      SubscriberList{Subscriber{nullptr, [](void*, const LogRecord&) {}}});
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module}, logger);
  scheduler.log(Level::info, "ARM log %d", 1);
  scheduler.log_statistics();
  logger.minimum(Level::debug);
  logger.counters();
  logger.reset();
  scheduler.run();
}
