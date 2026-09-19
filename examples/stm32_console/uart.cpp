#include "uart.h"

#include "statistics.h"

extern "C" UART_HandleTypeDef huart3;

namespace {
  // HAL callbacks have no user context. This route owns no application state.
  app::UartConsole* active_uart = nullptr;
}  // namespace

namespace app {
  bool UartConsole::Driver::start(const std::uint8_t* bytes, std::size_t size) {
    return board::StartTransmit(bytes, size);
  }

  UartConsole::UartConsole(Platform& platform)
      : input_(platform),
        output_(platform, driver_, board::tx_storage),
        display_(this, [](void* context, std::string_view text) {
          static_cast<UartConsole*>(context)->output_.write(text);
        }) {}

  core::Status UartConsole::init(core::InitStage stage) {
    if (stage == core::InitStage::stage1) {
      // Keep received bytes in hardware while short critical sections mask
      // IRQs. One-byte IT reception still handles partial lines immediately.
      if (HAL_UARTEx_EnableFifoMode(&huart3) != HAL_OK) {
        return core::Status::initialization_failed;
      }
      active_uart = this;
      start_receive();
    }
    return daveos::console::Module<UartConsole, Event>::init(stage);
  }

  void UartConsole::start_receive() {
    if (HAL_UART_Receive_IT(&huart3, &rx_byte_, 1) != HAL_OK) {
      Error_Handler();
    }
  }

  void UartConsole::received() {
    if (huart3.ErrorCode & ~HAL_UART_ERROR_DMA) {
      input_.error();
    } else {
      input_.receive(static_cast<char>(rx_byte_));
    }
    start_receive();
  }

  void UartConsole::error() {
    if (huart3.ErrorCode & HAL_UART_ERROR_DMA) {
      output_.error();
    }
    if (huart3.ErrorCode & ~HAL_UART_ERROR_DMA) {
      input_.error();
    }
    // Overrun ends reception; other line errors can leave it active.
    if (huart3.RxState == HAL_UART_STATE_READY) {
      start_receive();
    }
  }

  void UartConsole::output(const core::LogRecord& record) {
    display_.before_log();
    core::LogPrefix prefix(record);
    output_.write(prefix.view());
    output_.write(record.message);
    output_.write("\r\n");
    display_.after_log();
    output_.flush();
  }

  bool UartConsole::poll_line(console::Line& line) {
    const bool pending = input_.pop(line);
    if (pending) {
      display_.clear();
    } else {
      display_.show(input_.preview());
    }
    output_.flush();
    return pending;
  }

  void UartConsole::stop() {
    HAL_NVIC_DisableIRQ(USART3_IRQn);
    HAL_NVIC_DisableIRQ(board::kDmaIrq);
    HAL_UART_AbortTransmit(&huart3);
    HAL_UART_AbortReceive(&huart3);
    active_uart = nullptr;
  }

  void UartConsole::log_statistics(core::SchedulerInterface<Event>& scheduler) {
    LogTx(scheduler, "TX DMA", output_.counters());
  }
}  // namespace app

extern "C" void HAL_UART_RxCpltCallback(UART_HandleTypeDef* uart) {
  if (uart == &huart3 && active_uart) {
    active_uart->received();
  }
}

extern "C" void HAL_UART_TxCpltCallback(UART_HandleTypeDef* uart) {
  if (uart == &huart3 && active_uart) {
    active_uart->complete();
  }
}

extern "C" void HAL_UART_ErrorCallback(UART_HandleTypeDef* uart) {
  if (uart == &huart3 && active_uart) {
    active_uart->error();
  }
}
