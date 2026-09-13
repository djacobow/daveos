#include "daveos/core/scheduler.h"
namespace {
using namespace daveos::core;
enum class Event { sample };
struct Example : Module<Example, Event> {
  Example() : Module("arm_compile") {}
  static constexpr auto tasks() {
    return std::array{TaskDescriptor<Example>{"tick", &Example::tick}};
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
  scheduler.run();
  scheduler.snapshot();
  daveos::core::ThreadSafeQueue<int, 8, CompilePlatform> queue(platform);
  queue.push(1);
}
