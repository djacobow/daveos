#include "daveos/core/scheduler.h"
#include "daveos/platform/stm32h5/platform.h"
#include "main.h"

namespace {
using namespace daveos::core;
daveos::platform::stm32h5::Platform* active_platform = nullptr;
enum class Event { unused };
class Blinker final : public Module<Blinker, Event> {
 public:
  Blinker() : Module("blinker") {}
  static constexpr auto tasks() {
    return std::array{TaskDescriptor<Blinker>{"toggle", &Blinker::toggle}};
  }
  Status init(InitStage stage) {
    return stage == InitStage::stage1
               ? scheduler().schedule(*this, &Blinker::toggle, 500000,
                                      Mode::repeat)
               : Status::ok;
  }
  void toggle() { HAL_GPIO_TogglePin(LD1_GPIO_Port, LD1_Pin); }
};
}  // namespace

extern "C" void TIM2_IRQHandler() {
  if (active_platform) active_platform->interrupt();
}

extern "C" void DaveOS_Run() {
  daveos::platform::stm32h5::Platform platform;
  active_platform = &platform;
  // With TIMPRE disabled: TIM2 runs at PCLK1 for APB /1, otherwise 2*PCLK1.
  __HAL_RCC_TIMCLKPRESCALER(RCC_TIMPRES_DEACTIVATED);
  RCC_ClkInitTypeDef clocks{};
  std::uint32_t latency;
  HAL_RCC_GetClockConfig(&clocks, &latency);
  auto timer_hz = HAL_RCC_GetPCLK1Freq();
  if (clocks.APB1CLKDivider != RCC_HCLK_DIV1) timer_hz *= 2;
  if (platform.init(timer_hz) != Status::ok) Error_Handler();
  Blinker blinker;
  auto scheduler = make_scheduler<Event>(platform, ModuleList{&blinker});
  scheduler.run();
  platform.quiesce();
  active_platform = nullptr;
  Error_Handler();
}
