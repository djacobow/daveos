#include <cstdio>

#include "daveos/core/command.h"
#include "daveos/core/log_format.h"
#include "daveos/core/logger.h"
#include "daveos/core/scheduler.h"
#include "daveos/platform/stm32h7/platform.h"
#include "input.h"
#include "main.h"

extern "C" UART_HandleTypeDef huart3;

namespace app {
using namespace daveos::core;
using Platform = daveos::platform::stm32h7::Platform;
Platform* active_platform = nullptr;
Input<Platform>* active_input = nullptr;
std::uint8_t rx_byte;
enum class Event {};
void Write(std::string_view text) {
  HAL_UART_Transmit(&huart3, reinterpret_cast<const std::uint8_t*>(text.data()),
                    static_cast<std::uint16_t>(text.size()), 100);
}
class Console final : public Module<Console, Event> {
 public:
  explicit Console(Input<Platform>& input) : input_(input) {}
  static constexpr const char* name() { return "board"; }
  static constexpr auto tasks() {
    return std::array{TaskDescriptor<Console>{"input", &Console::Poll}};
  }
  static constexpr auto commands() {
    return std::array{
        DAVEOS_COMMAND(Console, "led", Led, "led <1|2|3> <on|off|toggle>"),
        DAVEOS_COMMAND(Console, "button", Button, "Read BTN1 (PC13)"),
        DAVEOS_COMMAND(Console, "stats", Stats, "Log scheduler statistics")};
  }
  template <typename Dispatcher>
  void dispatcher(Dispatcher& dispatcher) {
    dispatcher_ = &dispatcher;
    dispatch_ = [](void* context, std::string_view line) {
      return static_cast<Dispatcher*>(context)->dispatch(line);
    };
  }
  Status init(InitStage stage) {
    if (stage != InitStage::stage1) return Status::ok;
    I_("DaveOS STM32H755 M7; type help");
    return scheduler().schedule(*this, &Console::Poll, 1000, Mode::repeat);
  }

  void output(const LogRecord& record) {
    display_.before_log();
    LogPrefix prefix(record);
    Write(prefix.view());
    Write(record.message);
    Write("\r\n");
    display_.after_log();
  }

 private:
  void Poll() {
    [[maybe_unused]] auto dropped = input_.take_dropped();
    if (dropped)
      W_("Dropped %lu input lines/errors", static_cast<unsigned long>(dropped));
    Line line;
    if (input_.pop(line)) {
      display_.clear();
      if (line.size)
        I_("> %.*s", static_cast<int>(line.size), line.bytes.data());
      dispatch_(dispatcher_, line.view());
    } else {
      display_.show(input_.preview());
    }
  }
  Status Led(CommandArguments args) {
    if (args.size() != 2 || args[0].size() != 1 || args[0][0] < '1' ||
        args[0][0] > '3')
      return Status::invalid_argument;
    constexpr std::array<std::uint16_t, 3> pins{LD1_Pin, LD2_Pin, LD3_Pin};
    const std::array<GPIO_TypeDef*, 3> ports{LD1_GPIO_Port, LD2_GPIO_Port,
                                             LD3_GPIO_Port};
    auto index = static_cast<std::size_t>(args[0][0] - '1');
    if (args[1] == "toggle")
      HAL_GPIO_TogglePin(ports[index], pins[index]);
    else if (args[1] == "on" || args[1] == "off")
      HAL_GPIO_WritePin(ports[index], pins[index],
                        args[1] == "on" ? GPIO_PIN_SET : GPIO_PIN_RESET);
    else
      return Status::invalid_argument;
    I_("LED %c %.*s", args[0][0], static_cast<int>(args[1].size()),
       args[1].data());
    return Status::ok;
  }
  Status Button(CommandArguments args) {
    if (!args.empty()) return Status::invalid_argument;
    [[maybe_unused]] auto state = HAL_GPIO_ReadPin(BTN1_GPIO_Port, BTN1_Pin);
    I_("BTN1: %s", state == GPIO_PIN_SET ? "high" : "low");
    return Status::ok;
  }
  Status Stats(CommandArguments args) {
    if (!args.empty()) return Status::invalid_argument;
    scheduler().log_statistics();
    return Status::ok;
  }
  Input<Platform>& input_;
  LineDisplay display_{Write};
  void* dispatcher_ = nullptr;
  Status (*dispatch_)(void*, std::string_view) = nullptr;
};
void Output(void* context, const LogRecord& record) {
  static_cast<Console*>(context)->output(record);
}

void Receive() {
  if (HAL_UART_Receive_IT(&huart3, &rx_byte, 1) != HAL_OK) Error_Handler();
}
}  // namespace app

extern "C" void TIM2_IRQHandler() {
  if (app::active_platform) app::active_platform->interrupt();
}
extern "C" void HAL_UART_RxCpltCallback(UART_HandleTypeDef* uart) {
  if (uart != &huart3 || !app::active_input) return;
  if (uart->ErrorCode != HAL_UART_ERROR_NONE)
    app::active_input->error();
  else
    app::active_input->receive(static_cast<char>(app::rx_byte));
  app::Receive();
}
extern "C" void HAL_UART_ErrorCallback(UART_HandleTypeDef* uart) {
  if (uart != &huart3 || !app::active_input) return;
  app::active_input->error();
  // Overrun ends reception; other line errors can leave it active.
  if (uart->RxState == HAL_UART_STATE_READY) app::Receive();
}
extern "C" void DaveOS_Run() {
  using namespace daveos::core;
  app::Platform platform;
  app::active_platform = &platform;
  __HAL_RCC_TIMCLKPRESCALER(RCC_TIMPRES_DESACTIVATED);
  RCC_ClkInitTypeDef clocks{};
  std::uint32_t latency;
  HAL_RCC_GetClockConfig(&clocks, &latency);
  auto timer_hz = HAL_RCC_GetPCLK1Freq();
  if (clocks.APB1CLKDivider != RCC_APB1_DIV1) timer_hz *= 2;
  if (platform.init(timer_hz) != Status::ok) Error_Handler();
  app::Input input(platform);
  app::Console console(input);
  auto modules = ModuleList{&console};
  auto logger =
      make_logger(platform, SubscriberList{Subscriber{&console, app::Output}});
  auto scheduler = make_scheduler<app::Event>(platform, modules, logger);
  CommandDispatcher dispatcher(modules, scheduler);
  console.dispatcher(dispatcher);
  app::active_input = &input;
  app::Receive();
  scheduler.run();
  // Initialization failure is terminal; detach ISR state before unwinding.
  HAL_NVIC_DisableIRQ(USART3_IRQn);
  HAL_UART_AbortReceive(&huart3);
  app::active_input = nullptr;
  platform.quiesce();
  app::active_platform = nullptr;
  Error_Handler();
}
