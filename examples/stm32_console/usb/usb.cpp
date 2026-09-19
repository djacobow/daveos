#include "usb.hpp"

#include "core/logging/log_format.hpp"
#include "usb_device.h"

namespace {
  // The STM32 C middleware offers no user-context parameter for these
  // callbacks. This pointer routes them to an application-owned transport; it
  // owns no state.
  board::UsbTransport* active_usb = nullptr;
}  // namespace

namespace board {


  UsbTransport::UsbTransport(Platform& platform)
      : platform_(platform),
        input_(platform),
        output_(platform, driver_, storage_),
        display_(this, [](void* context, std::string_view text) {
          static_cast<UsbTransport*>(context)->Write(text);
        }) {}

  UsbTransport::~UsbTransport() { stop(); }

  bool UsbTransport::Driver::start(const std::uint8_t* bytes,
                                   std::size_t size) {
    return UsbDeviceTransmit(bytes, static_cast<std::uint32_t>(size));
  }

  bool UsbTransport::init() {
    if (active_usb || attempted_) {
      return false;
    }
    attempted_ = true;
    active_usb = this;
    if (UsbDeviceInit()) {
      return true;
    }
    stop();
    return false;
  }

  void UsbTransport::stop() {
    if (active_usb != this) {
      return;
    }
    UsbDeviceStop();
    active_usb = nullptr;
  }

  void UsbTransport::ResetDisplay() {
    if (!reset_display_) {
      return;
    }
    display_.reset();
    reset_display_ = false;
  }

  bool UsbTransport::poll_line(daveos::console::Line& line) {
    daveos::core::Guard guard(platform_);
    if (active_usb != this || !UsbDeviceReady()) {
      return false;
    }
    ResetDisplay();
    const bool pending = input_.pop(line);
    if (pending) {
      display_.clear();
    } else {
      display_.show(input_.preview());
    }
    output_.flush();
    return pending;
  }

  void UsbTransport::output(const daveos::core::LogRecord& record) {
    daveos::core::Guard guard(platform_);
    if (active_usb != this || !UsbDeviceReady()) {
      return;
    }
    ResetDisplay();
    display_.before_log();
    daveos::core::LogPrefix prefix(record);
    Write(prefix.view());
    Write(record.message);
    Write("\r\n");
    display_.after_log();
    output_.flush();
  }

  void UsbTransport::receive(const std::uint8_t* bytes, std::uint32_t size) {
    for (std::uint32_t i = 0; i < size; ++i) {
      input_.receive(static_cast<char>(bytes[i]));
    }
  }

  void UsbTransport::reset() {
    daveos::core::Guard guard(platform_);
    input_.reset();
    output_.discard();
    reset_display_ = true;
  }


}  // namespace board

extern "C" void UsbReceive(const std::uint8_t* bytes, std::uint32_t size) {
  if (active_usb) {
    active_usb->receive(bytes, size);
  }
}

extern "C" void UsbTransmitComplete() {
  if (active_usb) {
    active_usb->complete();
  }
}

extern "C" void UsbSessionReset() {
  if (active_usb) {
    active_usb->reset();
  }
}
