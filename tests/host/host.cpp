#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <semaphore>
#include <thread>

#include "platform/host/platform.h"
#include "support.hpp"
using namespace testing;
using Host = daveos::platform::host::Platform;
using namespace std::chrono_literals;

TEST_CASE(
    "socket thread triggers a concurrent interrupt that schedules task work") {
  Host platform;
  TestModule module;
  Sink sink;
  auto logger = make_logger(platform, SubscriberList{sink.subscriber()});
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module}, logger);
  int sockets[2];
  REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
  std::binary_semaphore callback_started(0), interrupt_done(0);
  std::atomic<bool> concurrent = false, in_callback = false;
  std::atomic<Status> schedule_status{Status::not_found};
  module.first_action = [&] {
    in_callback = true;
    callback_started.release();
    char byte = 'x';
    ::write(sockets[0], &byte, 1);
    CHECK(interrupt_done.try_acquire_for(2s));
    scheduler.log(Level::info, "task still active");
    in_callback = false;
  };
  module.second_action = [&] { scheduler.stop(); };
  struct Context {
    TestModule* module;
    std::atomic<bool>* concurrent;
    std::atomic<bool>* in_callback;
    std::atomic<Status>* status;
  } context{&module, &concurrent, &in_callback, &schedule_status};
  std::thread peer([&] {
    callback_started.acquire();
    char byte;
    if (::read(sockets[1], &byte, 1) == 1) {
      platform.interrupt(
          [](void* argument) {
            auto& context = *static_cast<Context*>(argument);
            *context.concurrent = context.in_callback->load();
            context.module->scheduler().log(Level::warning, "socket interrupt");
            *context.status = context.module->scheduler().schedule(
                *context.module, &TestModule::second, 0);
          },
          &context);
    }
    interrupt_done.release();
  });
  scheduler.schedule(module, &TestModule::first, 0);
  CHECK(scheduler.run() == Status::ok);
  peer.join();
  ::close(sockets[0]);
  ::close(sockets[1]);
  CHECK(concurrent);
  CHECK(schedule_status == Status::ok);
#if DAVEOS_LOGGING
  REQUIRE(sink.records.size() == 2);
  CHECK(sink.records[0].task == "interrupt");
  CHECK(sink.records[1].task == "first");
#else
  CHECK(sink.records.empty());
#endif
}

TEST_CASE("host timer runs in interrupt context and finishes before shutdown") {
  Host platform;
  TestModule module;
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module});
  std::atomic<int> count = 0;
  std::atomic<bool> interrupt_context = false;
  timer_action = [&] {
    interrupt_context = platform.in_interrupt();
    ++count;
    scheduler.schedule(module, &TestModule::second, 0);
  };
  module.first_action = [&] { scheduler.timer(1000, Timer); };
  module.second_action = [&] { scheduler.stop(); };
  scheduler.schedule(module, &TestModule::first, 0);
  CHECK(scheduler.run() == Status::ok);
  CHECK(count == 1);
  CHECK(interrupt_context);
  CHECK(platform.interrupt([](void*) {}) == Status::not_running);
}

TEST_CASE("critical sections exclude new interrupts until outermost exit") {
  Host platform;
  std::binary_semaphore attempted(0), finished(0);
  std::atomic<bool> entered = false;
  platform.enter();
  platform.enter();
  std::thread caller([&] {
    attempted.release();
    platform.interrupt(
        [](void* argument) {
          *static_cast<std::atomic<bool>*>(argument) = true;
        },
        &entered);
    finished.release();
  });
  attempted.acquire();
  platform.leave();
  CHECK_FALSE(finished.try_acquire_for(10ms));
  CHECK_FALSE(entered);
  platform.leave();
  CHECK(finished.try_acquire_for(2s));
  caller.join();
  CHECK(entered);
}

TEST_CASE("interrupts serialize and retained notifications release idle") {
  Host platform;
  std::binary_semaphore first_started(0), release_first(0);
  std::atomic<int> active = 0, maximum = 0;
  struct Context {
    std::binary_semaphore* started;
    std::binary_semaphore* release;
    std::atomic<int>* active;
    std::atomic<int>* maximum;
  };
  Context first{&first_started, &release_first, &active, &maximum};
  auto handler = [](void* argument) {
    auto& context = *static_cast<Context*>(argument);
    int current = ++*context.active;
    if (current > context.maximum->load()) *context.maximum = current;
    if (context.started) context.started->release();
    if (context.release) context.release->acquire();
    --*context.active;
  };
  std::thread one([&] { platform.interrupt(handler, &first); });
  first_started.acquire();
  Context second{nullptr, nullptr, &active, &maximum};
  std::thread two([&] { platform.interrupt(handler, &second); });
  release_first.release();
  one.join();
  two.join();
  CHECK(maximum == 1);
  auto sequence = platform.sequence();
  platform.notify();
  platform.idle(kForever, true,
                sequence);  // Must return despite notify preceding idle.
}

TEST_CASE("shutdown waits for a timer callback that is already executing") {
  Host platform;
  TestModule module;
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&module});
  std::binary_semaphore timer_started(0), release_timer(0);
  std::atomic<bool> finished = false;
  timer_action = [&] {
    timer_started.release();
    release_timer.acquire();
    finished = true;
  };
  module.first_action = [&] {
    scheduler.timer(100, Timer);
    CHECK(timer_started.try_acquire_for(2s));
    scheduler.stop();
    release_timer.release();
  };
  scheduler.schedule(module, &TestModule::first, 0);
  CHECK(scheduler.run() == Status::ok);
  CHECK(finished);
}
