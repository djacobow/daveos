#include "usb.hpp"

#include <optional>

#include "core/logging/log_format.hpp"
#include "usb_device.h"

namespace {
using board::Platform;
struct Driver {
  bool start(const std::uint8_t* bytes, std::size_t size) {
    return UsbDeviceTransmit(bytes, static_cast<std::uint32_t>(size));
  }
};
using Output = app::DmaOutput<Platform, Driver, 4096>;
void Write(std::string_view text);
struct Console {
  Platform& platform;
  app::Input<Platform> input;
  Driver driver;
  std::array<std::uint8_t, 8192> storage{};
  Output output;
  app::LineDisplay display{Write};
  bool reset_display = false;
  explicit Console(Platform& p)
      : platform(p), input(p), output(p, driver, storage) {}
};
// Fixed storage, constructed before USB IRQs are enabled. No heap allocation.
std::optional<Console> console;
void Write(std::string_view text) { console->output.write(text); }
void ResetDisplay() {
  if (console->reset_display) {
    console->display = app::LineDisplay{Write};
    console->reset_display = false;
  }
}
}  // namespace
namespace board {


bool InitUsb(Platform& platform) {
  console.emplace(platform);
  return UsbDeviceInit();
}
void StopUsb() {
  UsbDeviceStop();
  console.reset();
}
bool PollUsb(app::Line& line) {
  daveos::core::Guard guard(console->platform);
  ResetDisplay();
  if (!UsbDeviceReady()) return false;
  const bool pending = console->input.pop(line);
  if (pending)
    console->display.clear();
  else
    console->display.show(console->input.preview());
  console->output.flush();
  return pending;
}
void OutputUsb(const daveos::core::LogRecord& record) {
  daveos::core::Guard guard(console->platform);
  ResetDisplay();
  if (!UsbDeviceReady()) return;
  console->display.before_log();
  daveos::core::LogPrefix prefix(record);
  Write(prefix.view());
  Write(record.message);
  Write("\r\n");
  console->display.after_log();
  console->output.flush();
}
app::TxCounters UsbCounters() { return console->output.counters(); }
std::uint32_t UsbDroppedInput() { return console->input.take_dropped(); }


}  // namespace board
extern "C" void UsbReceive(const std::uint8_t* bytes, std::uint32_t size) {
  if (!console) return;
  for (std::uint32_t i = 0; i < size; ++i)
    console->input.receive(static_cast<char>(bytes[i]));
}
extern "C" void UsbTransmitComplete() {
  if (console) console->output.complete();
}
extern "C" void UsbSessionReset() {
  if (!console) return;
  daveos::core::Guard guard(console->platform);
  console->input.reset();
  console->output.discard();
  console->reset_display = true;
}
