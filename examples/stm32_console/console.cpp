#include <inttypes.h>

#include <charconv>
#include <cstdio>

#include "board_config.h"
#include "console/module.hpp"
#include "console/output.hpp"
#include "core/command/command.hpp"
#include "core/logging/log_format.hpp"
#include "core/logging/logger.hpp"
#include "core/schedule/scheduler.hpp"
#include "main.h"

#if DAVEOS_USB_CDC
#include "usb.hpp"
#endif

#if DAVEOS_NETWORKING
#include "net/module.hpp"
#include "platform/stm32/ethernet/driver.h"
#if DAVEOS_TCP_CONSOLE
#include "tcp_console.hpp"
#endif
#endif

extern "C" UART_HandleTypeDef huart3;

namespace app {
using namespace daveos::core;
using namespace daveos::console;
using Platform = board::Platform;
Platform* active_platform = nullptr;
#if DAVEOS_UART_CONSOLE
// HAL callbacks carry no application context; this pointer only routes IRQs.
class UartConsole;
UartConsole* active_uart = nullptr;

struct TxDriver {
  bool start(const std::uint8_t* bytes, std::size_t size) {
    return board::StartTransmit(bytes, size);
  }
};
using Tx = BufferedOutput<Platform, TxDriver, 4096>;
using UartInput = Input<Platform, 16>;
#endif
enum class Event {};
void BindCommands();

class Board final : public daveos::core::Module<Board, Event> {
 public:
  explicit Board(Platform& platform
#if DAVEOS_UART_CONSOLE
                 ,
                 Tx& tx
#endif
#if DAVEOS_USB_CDC
                 ,
                 board::UsbTransport& usb
#endif
                 )
      : platform_(platform)
#if DAVEOS_UART_CONSOLE
        ,
        tx_(tx)
#endif
#if DAVEOS_USB_CDC
        ,
        usb_(usb)
#endif
  {
  }
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
    if (stage == InitStage::stage2) BindCommands();
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
    // DaveOS timers take plain callbacks; route completion to this app-owned
    // board.
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
    [[maybe_unused]] auto tx = tx_.counters();
    I_("TX DMA: %s bytes, %" PRIu32 " transfers, %" PRIu32
       " dropped frames, %" PRIu32 " errors",
       LogUnsigned(tx.sent_bytes).c_str(), tx.transfers, tx.dropped_frames,
       tx.errors);
#endif
#if DAVEOS_USB_CDC
    [[maybe_unused]] auto usb = usb_.counters();
    I_("USB TX: %s bytes, %" PRIu32 " transfers, %" PRIu32
       " dropped frames, %" PRIu32 " errors",
       LogUnsigned(usb.sent_bytes).c_str(), usb.transfers, usb.dropped_frames,
       usb.errors);
#endif
    return Status::ok;
  }
  inline static Board* timer_owner_ = nullptr;
  Platform& platform_;
#if DAVEOS_UART_CONSOLE
  Tx& tx_;
#endif
#if DAVEOS_USB_CDC
  board::UsbTransport& usb_;
#endif
};
#if DAVEOS_UART_CONSOLE
class UartConsole final : public daveos::console::Module<UartConsole, Event> {
 public:
  UartConsole(UartInput& input, Tx& output)
      : input_(input),
        output_(output),
        display_(this, [](void* context, std::string_view text) {
          static_cast<UartConsole*>(context)->output_.write(text);
        }) {}
  Status init(InitStage stage) {
    if (stage == InitStage::stage1) {
      // Keep received bytes in hardware while short critical sections mask
      // IRQs. One-byte IT reception still handles partial lines immediately.
      if (HAL_UARTEx_EnableFifoMode(&huart3) != HAL_OK)
        return Status::initialization_failed;
      active_uart = this;
      start_receive();
    }
    return daveos::console::Module<UartConsole, Event>::init(stage);
  }
  void start_receive() {
    if (HAL_UART_Receive_IT(&huart3, &rx_byte_, 1) != HAL_OK) Error_Handler();
  }
  void received() {
    if (huart3.ErrorCode & ~HAL_UART_ERROR_DMA)
      input_.error();
    else
      input_.receive(static_cast<char>(rx_byte_));
    start_receive();
  }
  void complete() { output_.complete(); }
  void error() {
    if (huart3.ErrorCode & HAL_UART_ERROR_DMA) output_.error();
    if (huart3.ErrorCode & ~HAL_UART_ERROR_DMA) input_.error();
    // Overrun ends reception; other line errors can leave it active.
    if (huart3.RxState == HAL_UART_STATE_READY) start_receive();
  }
  static constexpr const char* name() { return "uart"; }
  std::uint32_t take_dropped() { return input_.take_dropped(); }
  void output(const LogRecord& record) {
    display_.before_log();
    LogPrefix prefix(record);
    output_.write(prefix.view());
    output_.write(record.message);
    output_.write("\r\n");
    display_.after_log();
    output_.flush();
  }
  bool poll_line(Line& line) {
    const bool pending = input_.pop(line);
    if (pending)
      display_.clear();
    else
      display_.show(input_.preview());
    output_.flush();
    return pending;
  }

 private:
  UartInput& input_;
  Tx& output_;
  LineDisplay display_;
  std::uint8_t rx_byte_ = 0;
};
#endif
}  // namespace app

extern "C" void TIM2_IRQHandler() {
  if (app::active_platform) app::active_platform->interrupt();
}

#if DAVEOS_UART_CONSOLE
extern "C" void HAL_UART_RxCpltCallback(UART_HandleTypeDef* uart) {
  if (uart == &huart3 && app::active_uart) app::active_uart->received();
}
extern "C" void HAL_UART_TxCpltCallback(UART_HandleTypeDef* uart) {
  if (uart == &huart3 && app::active_uart) app::active_uart->complete();
}
extern "C" void HAL_UART_ErrorCallback(UART_HandleTypeDef* uart) {
  if (uart == &huart3 && app::active_uart) app::active_uart->error();
}
#endif
namespace app {
// Static storage keeps long-lived buffers off the MCU stack. Constructors
// store references and metadata; hardware setup and wiring happen during init.
app::Platform platform;
#if DAVEOS_UART_CONSOLE
app::TxDriver driver;
app::Tx output(platform, driver, board::tx_storage);
UartInput input(platform);
app::UartConsole uart(input, output);
#endif
#if DAVEOS_USB_CDC
board::UsbTransport usb_transport(platform);
board::UsbConsole<app::Event> usb(usb_transport);
#endif
#if DAVEOS_NETWORKING
daveos::net::Config NetworkConfig() {
  auto config = daveos::net::stm32::board_network_config();
  // To use a static address, set dhcp=false and address/netmask/gateway here.
  return config;
}
daveos::net::Service network(daveos::net::stm32::ethernet_driver(),
                             {&platform,
                              [](void* p) -> std::uint32_t {
                                return static_cast<app::Platform*>(p)->now() /
                                       1000;
                              }},
                             {});
daveos::net::Module<app::Event> network_module(network, NetworkConfig);
#if DAVEOS_TCP_CONSOLE
app::TcpConsole<app::Event, app::Platform> tcp(platform, network);
#endif
#endif
app::Board board_module(platform
#if DAVEOS_UART_CONSOLE
                        ,
                        output
#endif
#if DAVEOS_USB_CDC
                        ,
                        usb_transport
#endif
);
auto modules = ModuleList {
  &board_module,
#if DAVEOS_NETWORKING
      &network_module,
#if DAVEOS_TCP_CONSOLE
      &tcp,
#endif
#endif
#if DAVEOS_UART_CONSOLE
      &uart,
#endif
#if DAVEOS_USB_CDC
      &usb,
#endif
};
auto subscribers = SubscriberList {
#if DAVEOS_NETWORKING && DAVEOS_TCP_CONSOLE
  tcp.subscriber(),
#endif
#if DAVEOS_UART_CONSOLE
      uart.subscriber(),
#endif
#if DAVEOS_USB_CDC
      usb.subscriber(),
#endif
};
auto logger = make_logger(platform, subscribers);
auto scheduler = make_scheduler<app::Event>(platform, modules, logger);
CommandDispatcher dispatcher(modules, scheduler);

void BindCommands() {
  auto sources = CommandSourceList {
#if DAVEOS_NETWORKING && DAVEOS_TCP_CONSOLE
    tcp.command_source(),
#endif
#if DAVEOS_UART_CONSOLE
        uart.command_source(),
#endif
#if DAVEOS_USB_CDC
        usb.command_source(),
#endif
  };
  dispatcher.bind_sources(sources);
}
}  // namespace app

extern "C" void DaveOS_Run() {
  using namespace app;
  active_platform = &platform;
  if (platform.init(board::TimerClock()) != Status::ok) Error_Handler();
  scheduler.run();
#if DAVEOS_NETWORKING
#if DAVEOS_TCP_CONSOLE
  tcp.stop();
#endif
  network.stop();
#endif
#if DAVEOS_USB_CDC
  usb_transport.stop();
#endif
  // Initialization failure is terminal; detach ISR state before halting.
#if DAVEOS_UART_CONSOLE
  HAL_NVIC_DisableIRQ(USART3_IRQn);
  HAL_NVIC_DisableIRQ(board::kDmaIrq);
  HAL_UART_AbortTransmit(&huart3);
  HAL_UART_AbortReceive(&huart3);
  app::active_uart = nullptr;
#endif
  platform.quiesce();
  app::active_platform = nullptr;
  Error_Handler();
}
