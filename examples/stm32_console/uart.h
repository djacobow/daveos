#pragma once

#include "application.h"
#include "console/module.hpp"
#include "console/output.hpp"

namespace app {
  namespace console = daveos::console;

  // USART3 owns its input queue and TX frame buffer; DMA storage is
  // board-owned.
  class UartConsole final : public console::Module<UartConsole, Event> {
   public:
    explicit UartConsole(Platform& platform);

    static constexpr const char* name() { return "uart"; }

    core::Status init(core::InitStage stage);
    void stop();
    void log_statistics(core::SchedulerInterface<Event>& scheduler);
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
  };
}  // namespace app
