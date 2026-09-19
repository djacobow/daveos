#include "uart.hpp"


extern "C" UART_HandleTypeDef huart3;

namespace {
  // HAL callbacks have no user context. This route owns no application state.
  daveos::platform::stm32::UartTransport* active_uart = nullptr;
}  // namespace

namespace daveos::platform::stm32 {
  bool UartTransport::Driver::start(const std::uint8_t* bytes,
                                    std::size_t size) {
    return board::StartTransmit(bytes, size);
  }

  UartTransport::UartTransport(Platform& platform)
      : input_(platform),
        output_(platform, driver_, board::tx_storage),
        display_(this, [](void* context, std::string_view text) {
          static_cast<UartTransport*>(context)->output_.write(text);
        }) {}

  bool UartTransport::init() {
    if (active_uart || attempted_) {
      return false;
    }
    attempted_ = true;
    if (HAL_UARTEx_EnableFifoMode(&huart3) != HAL_OK) {
      return false;
    }
    active_uart = this;
    start_receive();
    return true;
  }

  void UartTransport::start_receive() {
    if (HAL_UART_Receive_IT(&huart3, &rx_byte_, 1) != HAL_OK) {
      Error_Handler();
    }
  }

  void UartTransport::received() {
    if (huart3.ErrorCode & ~HAL_UART_ERROR_DMA) {
      input_.error();
    } else {
      input_.receive(static_cast<char>(rx_byte_));
    }
    start_receive();
  }

  void UartTransport::error() {
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

  void UartTransport::output(const core::LogRecord& record) {
    display_.before_log();
    core::LogPrefix prefix(record);
    output_.write(prefix.view());
    output_.write(record.message);
    output_.write("\r\n");
    display_.after_log();
    output_.flush();
  }

  bool UartTransport::poll_line(console::Line& line) {
    const bool pending = input_.pop(line);
    if (pending) {
      display_.clear();
    } else {
      display_.show(input_.preview());
    }
    output_.flush();
    return pending;
  }

  void UartTransport::stop() {
    if (active_uart != this) {
      return;
    }
    HAL_NVIC_DisableIRQ(USART3_IRQn);
    HAL_NVIC_DisableIRQ(board::kDmaIrq);
    HAL_UART_AbortTransmit(&huart3);
    HAL_UART_AbortReceive(&huart3);
    active_uart = nullptr;
  }

}  // namespace daveos::platform::stm32

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
