#include <string>

#include "catch_amalgamated.hpp"
#include "console/output.hpp"
#include "platform/fake/platform.h"

namespace {
  struct Driver {
    const std::uint8_t* bytes = nullptr;
    std::size_t size = 0;
    bool fail = false;

    bool start(const std::uint8_t* data, std::size_t length) {
      if (fail) {
        return false;
      }
      bytes = data;
      size = length;
      return true;
    }

    std::string sent() const {
      return {reinterpret_cast<const char*>(bytes), size};
    }
  };

  struct Fixture {
    daveos::platform::fake::Platform platform;
    Driver driver;
    std::array<std::uint8_t, 16> storage{};
    daveos::console::BufferedOutput<decltype(platform), Driver, 8, 12> output{
        platform, driver, storage};
  };

  using daveos::core::Status;
}  // namespace

TEST_CASE("UART DMA preserves active data while accumulating the next buffer") {
  Fixture f;
  std::string message = "first";
  f.output.write(message);
  REQUIRE(f.output.flush() == Status::ok);
  message = "other";
  CHECK(f.driver.sent() == "first");
  auto* first = f.driver.bytes;
  f.output.write("ab");
  REQUIRE(f.output.flush() == Status::ok);
  f.output.write("cd");
  REQUIRE(f.output.flush() == Status::ok);
  CHECK(f.driver.bytes == first);
  CHECK(f.driver.sent() == "first");
  f.output.complete();
  CHECK(f.driver.bytes != first);
  CHECK(f.driver.sent() == "abcd");
  f.output.write("next");
  REQUIRE(f.output.flush() == Status::ok);
  CHECK(f.driver.sent() == "abcd");
  f.output.complete();
  CHECK(f.driver.bytes == first);
  CHECK(f.driver.sent() == "next");
  f.output.complete();
  CHECK(f.output.queued() == 0);
  CHECK(f.output.counters().sent_bytes == 13);
  CHECK(f.output.counters().transfers == 3);
}

TEST_CASE("UART DMA overflow drops whole frames and recovers") {
  Fixture f;
  f.output.write("active");
  REQUIRE(f.output.flush() == Status::ok);
  f.output.write("pending");
  REQUIRE(f.output.flush() == Status::ok);
  f.output.write("bad");
  CHECK(f.output.flush() == Status::full);
  f.output.complete();
  CHECK(f.driver.sent() == "pending");
  f.output.write("1234567890123");
  CHECK(f.output.flush() == Status::full);
  f.output.write("ok");
  REQUIRE(f.output.flush() == Status::ok);
  f.output.complete();
  CHECK(f.driver.sent() == "ok");
  f.output.complete();
  CHECK(f.output.counters().dropped_frames == 2);
  CHECK(f.output.flush() == Status::ok);
  f.output.complete();
  CHECK(f.output.counters().transfers == 3);
}

TEST_CASE(
    "UART DMA start failures and transfer errors discard uncertain data") {
  Fixture f;
  f.driver.fail = true;
  f.output.write("failure");
  CHECK(f.output.flush() != Status::ok);
  CHECK(f.output.queued() == 0);
  f.driver.fail = false;
  f.output.write("active");
  REQUIRE(f.output.flush() == Status::ok);
  f.output.write("pending");
  REQUIRE(f.output.flush() == Status::ok);
  f.output.error();
  CHECK(f.output.queued() == 0);
  CHECK(f.output.counters().errors == 2);
  f.output.write("recovery");
  REQUIRE(f.output.flush() == Status::ok);
  CHECK(f.driver.sent() == "recovery");
  f.output.complete();
  CHECK(f.output.counters().sent_bytes == 8);
}

TEST_CASE(
    "Transport teardown discards all output and permits a fresh session") {
  Fixture f;
  f.output.write("active");
  REQUIRE(f.output.flush() == Status::ok);
  f.output.write("pending");
  REQUIRE(f.output.flush() == Status::ok);
  f.output.write("staged");
  // Simulate the driver releasing its active buffer before teardown.
  f.driver.bytes = nullptr;
  f.driver.size = 0;
  f.output.discard();
  CHECK(f.output.queued() == 0);
  f.output.complete();
  CHECK(f.output.counters().transfers == 0);
  REQUIRE(f.output.flush() == Status::ok);
  CHECK(f.driver.bytes == nullptr);
  f.output.write("fresh");
  REQUIRE(f.output.flush() == Status::ok);
  CHECK(f.driver.sent() == "fresh");
  f.output.complete();
  CHECK(f.output.counters().sent_bytes == 5);
  CHECK(f.output.counters().errors == 0);
}
