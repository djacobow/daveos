#include <inttypes.h>

#include <charconv>
#include <cstdio>

#include "board_config.h"
#include "core/command/command.hpp"
#include "core/logging/log_format.hpp"
#include "core/logging/logger.hpp"
#include "core/schedule/scheduler.hpp"
#include "input.hpp"
#include "main.h"
#include "output.hpp"

#if DAVEOS_USB_CDC
#include "usb.hpp"
#endif

extern "C" UART_HandleTypeDef huart3;

namespace app {
using namespace daveos::core;
using Platform = board::Platform;
Platform* active_platform = nullptr;
#if DAVEOS_UART_CONSOLE
Input<Platform>* active_input = nullptr;
std::uint8_t rx_byte;

struct TxDriver {
  bool start(const std::uint8_t* bytes, std::size_t size) {
    return board::StartTransmit(bytes, size);
  }
};
using Tx = DmaOutput<Platform, TxDriver, 4096>;
Tx* active_tx = nullptr;
void Write(std::string_view text) { active_tx->write(text); }
#endif
enum class Event {};

class Board final : public Module<Board, Event> {
 public:
  explicit Board(Platform& platform) : platform_(platform) {}
  static constexpr const char* name() { return "board"; }
  static constexpr auto commands() {
    return std::array{
        DAVEOS_COMMAND(Board, "led", Led, "led <1|2|3> <on|off|toggle>"),
        DAVEOS_COMMAND(Board, "button", Button, "Read button level"),
        DAVEOS_COMMAND(Board, "stats", Stats, "Log scheduler statistics"),
        DAVEOS_COMMAND(Board, "timer", Timer, "timer <microseconds>"),
        DAVEOS_COMMAND(Board, "reset", Reset, "Reset the MCU immediately")};
  }
  Status init(InitStage stage) {
    if (stage == InitStage::stage1) I_("DaveOS %s; type help", board::kName);
    return Status::ok;
  }

 private:
  Status Timer(CommandArguments args) {
    if (args.size() != 1 || args[0].empty()) return Status::invalid_argument;
    Time delay = 0;
    const auto text = args[0];
    const auto end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, delay);
    if (parsed.ec != std::errc{} || parsed.ptr != end || !delay)
      return Status::invalid_argument;
    // The board console is a singleton; DaveOS timers take plain callbacks.
    timer_owner_ = this;
    return scheduler().timer(delay, [] { timer_owner_->TimerFired(); });
  }
  void TimerFired() { I_("Timer fired"); }
  Status Reset(CommandArguments args) {
    if (!args.empty()) return Status::invalid_argument;
    return platform_.reset();
  }
  Status Led(CommandArguments args) {
    if (args.size() != 2 || args[0].size() != 1 || args[0][0] < '1' ||
        args[0][0] > '3')
      return Status::invalid_argument;
    auto index = static_cast<std::size_t>(args[0][0] - '1');
    if (args[1] == "toggle")
      board::ToggleLed(index);
    else if (args[1] == "on" || args[1] == "off")
      board::SetLed(index, args[1] == "on");
    else
      return Status::invalid_argument;
    I_("LED %c %.*s", args[0][0], static_cast<int>(args[1].size()),
       args[1].data());
    return Status::ok;
  }
  Status Button(CommandArguments args) {
    if (!args.empty()) return Status::invalid_argument;
    [[maybe_unused]] auto state = board::ReadButton();
    I_("BTN1: %s", state ? "high" : "low");
    return Status::ok;
  }
  Status Stats(CommandArguments args) {
    if (!args.empty()) return Status::invalid_argument;
    scheduler().log_statistics();
#if DAVEOS_UART_CONSOLE
    [[maybe_unused]] auto tx = active_tx->counters();
    I_("TX DMA: %s bytes, %" PRIu32 " transfers, %" PRIu32
       " dropped frames, %" PRIu32 " errors",
       LogUnsigned(tx.sent_bytes).c_str(), tx.transfers, tx.dropped_frames,
       tx.errors);
#endif
#if DAVEOS_USB_CDC
    [[maybe_unused]] auto usb = board::UsbCounters();
    I_("USB TX: %s bytes, %" PRIu32 " transfers, %" PRIu32
       " dropped frames, %" PRIu32 " errors",
       LogUnsigned(usb.sent_bytes).c_str(), usb.transfers, usb.dropped_frames,
       usb.errors);
#endif
    return Status::ok;
  }
  inline static Board* timer_owner_ = nullptr;
  Platform& platform_;
};
#if DAVEOS_UART_CONSOLE
class UartConsole final : public Module<UartConsole, Event> {
 public:
  explicit UartConsole(Input<Platform>& input) : input_(input) {}
  static constexpr const char* name() { return "uart"; }
  static constexpr auto tasks() {
    return std::array{TaskDescriptor<UartConsole>{"input", &UartConsole::Poll}};
  }
  Subscriber subscriber() {
    return {this, [](void* context, const LogRecord& record) {
              static_cast<UartConsole*>(context)->output(record);
            }};
  }
  CommandSource& command_source() { return source_; }
  Status init(InitStage stage) {
    if (stage != InitStage::stage1) return Status::ok;
    return scheduler().schedule(*this, &UartConsole::Poll, 1000, Mode::repeat);
  }

  void output(const LogRecord& record) {
    display_.before_log();
    LogPrefix prefix(record);
    Write(prefix.view());
    Write(record.message);
    Write("\r\n");
    display_.after_log();
    active_tx->flush();
  }

 private:
  void Poll() {
    [[maybe_unused]] auto dropped = input_.take_dropped();
    if (dropped) W_("Dropped %" PRIu32 " input lines/errors", dropped);
    Line line;
    if (input_.pop(line)) {
      display_.clear();
      Dispatch(line);
    } else {
      display_.show(input_.preview());
    }
    active_tx->flush();
  }
  void Dispatch(const Line& line) {
    if (line.size) I_("> %.*s", static_cast<int>(line.size), line.bytes.data());
    source_.dispatch(line.view());
  }
  Input<Platform>& input_;
  LineDisplay display_{Write};
  CommandSource source_;
};

void Receive() {
  if (HAL_UART_Receive_IT(&huart3, &rx_byte, 1) != HAL_OK) Error_Handler();
}
#endif
}  // namespace app

extern "C" void TIM2_IRQHandler() {
  if (app::active_platform) app::active_platform->interrupt();
}

#if DAVEOS_UART_CONSOLE
extern "C" void HAL_UART_RxCpltCallback(UART_HandleTypeDef* uart) {
  if (uart != &huart3 || !app::active_input) return;
  if (uart->ErrorCode & ~HAL_UART_ERROR_DMA)
    app::active_input->error();
  else
    app::active_input->receive(static_cast<char>(app::rx_byte));
  app::Receive();
}
extern "C" void HAL_UART_TxCpltCallback(UART_HandleTypeDef* uart) {
  if (uart == &huart3 && app::active_tx) app::active_tx->complete();
}
extern "C" void HAL_UART_ErrorCallback(UART_HandleTypeDef* uart) {
  if (uart != &huart3 || !app::active_input) return;
  if ((uart->ErrorCode & HAL_UART_ERROR_DMA) && app::active_tx)
    app::active_tx->error();
  if (uart->ErrorCode & ~HAL_UART_ERROR_DMA) app::active_input->error();
  // Overrun ends reception; other line errors can leave it active.
  if (uart->RxState == HAL_UART_STATE_READY) app::Receive();
}
#endif
extern "C" void DaveOS_Run() {
  using namespace daveos::core;
  app::Platform platform;
  app::active_platform = &platform;
  auto timer_hz = board::TimerClock();
  if (platform.init(timer_hz) != Status::ok) Error_Handler();
  app::Board board_module(platform);
#if DAVEOS_UART_CONSOLE
  app::TxDriver driver;
  app::Tx output(platform, driver, board::tx_storage);
  app::active_tx = &output;
  app::Input input(platform);
  app::UartConsole uart(input);
#endif
#if DAVEOS_USB_CDC
  board::UsbConsole<app::Event> usb;
#endif
  auto modules = ModuleList {
    &board_module,
#if DAVEOS_UART_CONSOLE
        &uart,
#endif
#if DAVEOS_USB_CDC
        &usb,
#endif
  };
  auto subscribers = SubscriberList {
#if DAVEOS_UART_CONSOLE
    uart.subscriber(),
#endif
#if DAVEOS_USB_CDC
        usb.subscriber(),
#endif
  };
  auto logger = make_logger(platform, subscribers);
  auto scheduler = make_scheduler<app::Event>(platform, modules, logger);
  auto sources = CommandSourceList {
#if DAVEOS_UART_CONSOLE
    uart.command_source(),
#endif
#if DAVEOS_USB_CDC
        usb.command_source(),
#endif
  };
  CommandDispatcher dispatcher(modules, scheduler, sources);
#if DAVEOS_UART_CONSOLE
  app::active_input = &input;
  app::Receive();
#endif
#if DAVEOS_USB_CDC
  if (!board::InitUsb(platform)) Error_Handler();
#endif
  scheduler.run();
#if DAVEOS_USB_CDC
  board::StopUsb();
#endif
  // Initialization failure is terminal; detach ISR state before unwinding.
#if DAVEOS_UART_CONSOLE
  HAL_NVIC_DisableIRQ(USART3_IRQn);
  HAL_NVIC_DisableIRQ(board::kDmaIrq);
  HAL_UART_AbortTransmit(&huart3);
  app::active_tx = nullptr;
  HAL_UART_AbortReceive(&huart3);
  app::active_input = nullptr;
#endif
  platform.quiesce();
  app::active_platform = nullptr;
  Error_Handler();
}
