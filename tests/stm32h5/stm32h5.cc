#include "catch_amalgamated.hpp"
#include "daveos/platform/stm32h5/platform.h"
#include "stm32h563xx.h"

using daveos::core::Status;
using daveos::core::Time;
using daveos::platform::stm32h5::Platform;
namespace {
struct Fixture {
  Platform platform;
  Fixture() {
    hardware::Reset();
    REQUIRE(platform.init(125000000) == Status::ok);
  }
  void Interrupt() {
    hardware::ipsr = 16 + TIM2_IRQn;
    hardware::pending = false;
    platform.interrupt();
    hardware::ipsr = 0;
  }
};
void Count(void* context) { ++*static_cast<int*>(context); }
}  // namespace

TEST_CASE_METHOD(Fixture, "STM32 initialization and nested interrupt masks") {
  REQUIRE(TIM2->PSC == 124);
  REQUIRE(platform.init(125000000) == Status::already_initialized);
  platform.enter();
  platform.enter();
  platform.leave();
  REQUIRE(hardware::mask == 1);
  platform.leave();
  REQUIRE(hardware::mask == 0);
  hardware::mask = 1;
  platform.enter();
  platform.leave();
  REQUIRE(hardware::mask == 1);
  platform.quiesce();
  REQUIRE_FALSE(hardware::enabled);
  REQUIRE(platform.init(123456789) == Status::invalid_argument);
}

TEST_CASE_METHOD(Fixture, "STM32 time accounts for pending overflow once") {
  TIM2->CNT = 0xfffffff0;
  REQUIRE(platform.now() == 0xfffffff0);
  TIM2->CNT = 3;
  TIM2->SR.flags = TIM_SR_UIF | TIM_SR_CC1IF;
  REQUIRE(platform.now() == (Time{1} << 32) + 3);
  REQUIRE(TIM2->SR.flags == TIM_SR_CC1IF);
  Interrupt();
  REQUIRE(platform.now() == (Time{1} << 32) + 3);
}

TEST_CASE_METHOD(Fixture,
                 "STM32 timer replacement cancellation and immediate expiry") {
  int count = 0;
  platform.arm(10, Count, &count);
  platform.arm(20, Count, &count);
  TIM2->CNT = 10;
  Interrupt();
  REQUIRE(count == 0);
  TIM2->CNT = 20;
  Interrupt();
  REQUIRE(count == 1);
  Interrupt();
  REQUIRE(count == 1);
  platform.arm(0, Count, &count);
  REQUIRE(hardware::pending);
  Interrupt();
  REQUIRE(count == 2);
  platform.arm(1, Count, &count);
  platform.disarm();
  TIM2->CNT = 30;
  Interrupt();
  REQUIRE(count == 2);
}

TEST_CASE_METHOD(Fixture, "STM32 long timer crosses counter rollover") {
  int count = 0;
  platform.arm((Time{1} << 32) + 50, Count, &count);
  REQUIRE(TIM2->CCR1 == 0x7fffffff);
  TIM2->CNT = 0x7fffffff;
  Interrupt();
  REQUIRE(count == 0);
  TIM2->CNT = 0xfffffffe;
  Interrupt();
  REQUIRE(TIM2->CCR1 == 50);
  TIM2->CNT = 0;
  TIM2->SR.flags = TIM_SR_UIF;
  Interrupt();
  REQUIRE(count == 0);
  TIM2->CNT = 50;
  Interrupt();
  REQUIRE(count == 1);
}

TEST_CASE_METHOD(Fixture,
                 "STM32 expiry while programming compare pends an interrupt") {
  int count = 0;
  hardware::compare_latency = 10;
  platform.arm(5, Count, &count);
  REQUIRE(hardware::pending);
  REQUIRE(count == 0);
  Interrupt();
  REQUIRE(count == 1);
}

TEST_CASE_METHOD(Fixture, "STM32 callbacks can rearm in interrupt context") {
  struct State {
    Platform* platform;
    int count = 0;
    static void Fire(void* argument) {
      auto& state = *static_cast<State*>(argument);
      REQUIRE(state.platform->in_interrupt());
      REQUIRE(std::string_view(state.platform->context().task) == "interrupt");
      REQUIRE(hardware::mask == 0);
      if (++state.count == 1) state.platform->arm(5, Fire, argument);
    }
  } state{&platform};
  platform.context({"module", "task"});
  platform.arm(1, State::Fire, &state);
  TIM2->CNT = 1;
  Interrupt();
  REQUIRE(TIM2->CCR1 == 6);
  TIM2->CNT = 6;
  Interrupt();
  REQUIRE(state.count == 2);
  REQUIRE(std::string_view(platform.context().task) == "task");
}

TEST_CASE_METHOD(Fixture,
                 "STM32 idle checks notifications and deadlines before sleep") {
  auto observed = platform.sequence();
  platform.notify();
  platform.idle(10, true, observed);
  REQUIRE(hardware::sleeps == 0);
  observed = platform.sequence();
  platform.idle(0, true, observed);
  platform.idle(10, false, observed);
  REQUIRE(hardware::sleeps == 0);
  SCB->SCR = SCB_SCR_SLEEPDEEP_Msk;
  platform.idle(10, true, observed);
  REQUIRE(hardware::sleeps == 1);
  REQUIRE(hardware::sleep_mask == 1);
  REQUIRE(hardware::mask == 0);
  REQUIRE(SCB->SCR == 0);
}
