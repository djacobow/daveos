#pragma once

#include "board_config.h"
#include "console/module.hpp"
#include "console/output.hpp"
#include "console/transport.hpp"

namespace daveos::platform::stm32 {
  namespace core = daveos::core;
  using Platform = board::Platform;
  namespace console = daveos::console;

  // USART3 owns its input queue and TX frame buffer; DMA storage is
  // board-owned.
  class UartTransport final {
   public:
    explicit UartTransport(Platform& platform);

    static constexpr const char* name() { return "uart"; }

    ~UartTransport() { stop(); }

    UartTransport(const UartTransport&) = delete;
    UartTransport& operator=(const UartTransport&) = delete;
    bool init();
    void stop();

    console::TxCounters counters() { return output_.counters(); }

    static constexpr const char* statistics_label() { return "TX DMA"; }

    void start_receive();
    void received();

    void complete() { output_.complete(); }

    void error();

    std::uint32_t take_dropped() { return input_.take_dropped(); }

    void output(const core::LogRecord& record);
    bool poll_line(console::Line& line);

   private:
    struct Driver {
      bool start(const std::uint8_t* bytes, std::size_t size);
    };

    Driver driver_;
    console::Input<Platform, 16> input_;
    console::BufferedOutput<Platform, Driver, 4096> output_;
    console::LineDisplay display_;
    std::uint8_t rx_byte_ = 0;
    bool attempted_ = false;
  };

  template <typename Event>
  using UartConsole = daveos::console::TransportModule<Event, UartTransport>;
}  // namespace daveos::platform::stm32
