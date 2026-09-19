#pragma once

#include "board_config.h"
#include "console/module.hpp"
#include "console/output.hpp"

namespace board {


// Application-owned USB transport. Only the C middleware callback route is
// global: one hardware USB device may be active at a time. All entry points
// except receive/complete/reset run on the scheduler thread. Stop quiesces
// callbacks before destruction; platform and DMA storage outlive transfers.
class UsbTransport {
 public:
  explicit UsbTransport(Platform& platform);
  ~UsbTransport();
  UsbTransport(const UsbTransport&) = delete;
  UsbTransport& operator=(const UsbTransport&) = delete;
  bool init();
  void stop();
  bool poll_line(daveos::console::Line& line);
  void output(const daveos::core::LogRecord& record);
  daveos::console::TxCounters counters() { return output_.counters(); }
  std::uint32_t take_dropped() { return input_.take_dropped(); }
  void receive(const std::uint8_t* bytes, std::uint32_t size);
  void complete() { output_.complete(); }
  void reset();

 private:
  struct Driver {
    bool start(const std::uint8_t* bytes, std::size_t size);
  };
  void ResetDisplay();
  void Write(std::string_view text) { output_.write(text); }
  Platform& platform_;
  daveos::console::Input<Platform> input_;
  Driver driver_;
  std::array<std::uint8_t, 8192> storage_{};
  daveos::console::BufferedOutput<Platform, Driver, 4096> output_;
  daveos::console::LineDisplay display_;
  bool reset_display_ = false, attempted_ = false;
};

// Independent USB module, borrowing the application's transport.
template <typename Event>
class UsbConsole final
    : public daveos::console::Module<UsbConsole<Event>, Event> {
 public:
  explicit UsbConsole(UsbTransport& transport) : transport_(transport) {}
  static constexpr const char* name() { return "usb"; }
  daveos::core::Status init(daveos::core::InitStage stage) {
    if (stage == daveos::core::InitStage::stage1 && !transport_.init())
      return daveos::core::Status::initialization_failed;
    return daveos::console::Module<UsbConsole<Event>, Event>::init(stage);
  }
  bool poll_line(daveos::console::Line& line) {
    return transport_.poll_line(line);
  }
  std::uint32_t take_dropped() { return transport_.take_dropped(); }
  void output(const daveos::core::LogRecord& record) {
    transport_.output(record);
  }

 private:
  UsbTransport& transport_;
};


}  // namespace board
