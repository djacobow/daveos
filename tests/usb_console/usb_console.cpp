#include "catch_amalgamated.hpp"
#include "examples/stm32_console/usb/usb.hpp"
#include "examples/stm32_console/usb/usb_device.h"

namespace {
  bool init_ok = true, ready = false;
  std::size_t starts = 0, stops = 0;

  void Feed(const char* text) {
    auto bytes = std::string_view(text);
    UsbReceive(reinterpret_cast<const std::uint8_t*>(bytes.data()),
               bytes.size());
  }
}  // namespace

extern "C" bool UsbDeviceInit() {
  ++starts;
  ready = init_ok;
  return init_ok;
}

extern "C" void UsbDeviceStop() {
  ++stops;
  ready = false;
}

extern "C" bool UsbDeviceReady() { return ready; }

extern "C" bool UsbDeviceTransmit(const std::uint8_t*, std::uint32_t) {
  return ready;
}

TEST_CASE("USB callback routing borrows one live application-owned transport") {
  board::Platform platform;
  starts = stops = 0;
  init_ok = true;
  board::UsbTransport first(platform), second(platform);
  REQUIRE(first.init());
  REQUIRE_FALSE(first.init());
  REQUIRE_FALSE(second.init());
  CHECK(starts == 1);
  Feed("first\n");
  daveos::console::Line line;
  REQUIRE(first.poll_line(line));
  CHECK(line.view() == "first");
  REQUIRE_FALSE(second.poll_line(line));
  Feed("stale");
  UsbSessionReset();
  Feed("fresh\n");
  REQUIRE(first.poll_line(line));
  CHECK(line.view() == "fresh");
  first.stop();
  Feed("unrouted\n");
  REQUIRE(second.init());
  Feed("second\n");
  REQUIRE(second.poll_line(line));
  CHECK(line.view() == "second");
  first.stop();  // Stopping a former owner cannot stop the current owner.
  CHECK(stops == 1);
  second.stop();
  CHECK(stops == 2);
}

TEST_CASE("USB initialization failure releases the callback route") {
  board::Platform platform;
  init_ok = false;
  board::UsbTransport failed(platform), next(platform);
  REQUIRE_FALSE(failed.init());
  init_ok = true;
  REQUIRE_FALSE(failed.init());
  REQUIRE(next.init());
  failed.stop();
  Feed("next\n");
  daveos::console::Line line;
  REQUIRE(next.poll_line(line));
  CHECK(line.view() == "next");
}
