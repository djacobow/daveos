#include <string>
#include <string_view>

#include "catch_amalgamated.hpp"
#include "console/input.hpp"
#include "platform/fake/platform.h"

namespace {
  using Platform = daveos::platform::fake::Platform;

  template <std::size_t Capacity>
  void Feed(daveos::console::Input<Platform, Capacity>& input,
            std::string_view bytes) {
    for (auto byte : bytes) {
      input.receive(byte);
    }
  }

  std::string_view View(const daveos::console::Line& line) {
    return {line.bytes.data(), line.size};
  }
}  // namespace

TEST_CASE("UART lines accept CR, LF and CRLF without duplicate commands") {
  Platform platform;
  daveos::console::Input input(platform);
  Feed(input, "one\rtwo\nthree\r\nfour");
  daveos::console::Line line;
  for (auto expected : {"one", "two", "three"}) {
    REQUIRE(input.pop(line));
    CHECK(View(line) == expected);
  }
  CHECK_FALSE(input.pop(line));
  Feed(input, "\n");
  REQUIRE(input.pop(line));
  CHECK(View(line) == "four");
}

TEST_CASE("UART overlength input remains one rejected line") {
  Platform platform;
  daveos::console::Input input(platform);
  for (int i = 0; i < 400; ++i) {
    input.receive('x');
  }
  Feed(input, "\nhelp\n");
  daveos::console::Line line;
  REQUIRE(input.pop(line));
  CHECK(line.size == 257);
  REQUIRE(input.pop(line));
  CHECK(View(line) == "help");
  CHECK_FALSE(input.pop(line));
}

TEST_CASE("UART queue overflow drops whole lines and recovers") {
  Platform platform;
  daveos::console::Input input(platform);
  Feed(input, "a\nb\nc\nd\nbad\n");
  CHECK(input.take_dropped() == 1);
  CHECK(input.take_dropped() == 0);
  daveos::console::Line line;
  for (auto expected : {"a", "b", "c", "d"}) {
    REQUIRE(input.pop(line));
    CHECK(View(line) == expected);
  }
  Feed(input, "good\n");
  REQUIRE(input.pop(line));
  CHECK(View(line) == "good");
}

TEST_CASE("UART errors discard the damaged line through its terminator") {
  Platform platform;
  daveos::console::Input input(platform);
  Feed(input, "board led ");
  input.error();
  input.error();
  Feed(input, "1 on\r\nhelp\n");
  daveos::console::Line line;
  REQUIRE(input.pop(line));
  CHECK(View(line) == "help");
  CHECK_FALSE(input.pop(line));
  CHECK(input.take_dropped() == 1);
}

TEST_CASE("UART preview tracks editing and clears after Return or error") {
  Platform platform;
  daveos::console::Input input(platform);
  Feed(input,
       "helx\bp\x7f"
       "p");
  CHECK(input.preview().view() == "help");
  Feed(input, "\r\n");
  CHECK(input.preview().view().empty());
  daveos::console::Line line;
  REQUIRE(input.pop(line));
  CHECK(line.view() == "help");
  Feed(input, "bad");
  input.error();
  CHECK(input.preview().view().empty());
}

namespace {
  std::string terminal_output;

  void TerminalWrite(std::string_view text) { terminal_output += text; }
}  // namespace

TEST_CASE("UART echo clears on submit and preserves input around log output") {
  terminal_output.clear();
  daveos::console::LineDisplay display(
      nullptr, [](void*, std::string_view text) { TerminalWrite(text); });
  daveos::console::Line line;
  line.bytes[0] = 'h';
  line.size = 1;
  display.show(line);
  CHECK(terminal_output == "h");
  display.show(line);
  CHECK(terminal_output == "h");
  line.bytes[1] = 'i';
  line.size = 2;
  display.show(line);
  CHECK(terminal_output == "hi");
  terminal_output.clear();
  display.before_log();
  TerminalWrite("log\r\n");
  display.after_log();
  CHECK(terminal_output == "\r\x1b[2Klog\r\nhi");
  terminal_output.clear();
  display.clear();
  CHECK(terminal_output == "\r\x1b[2K");
  display.after_log();
  CHECK(terminal_output == "\r\x1b[2K");
}

TEST_CASE(
    "Transport reset discards queued and partial input and parser state") {
  Platform platform;
  daveos::console::Input input(platform);
  Feed(input, "queued\npartial");
  input.error();
  input.reset();
  daveos::console::Line line;
  CHECK_FALSE(input.pop(line));
  CHECK(input.preview().view().empty());
  CHECK(input.take_dropped() == 1);
  Feed(input, "fresh\r");
  input.reset();
  Feed(input, "\n");
  REQUIRE(input.pop(line));
  CHECK(line.view().empty());
}

TEST_CASE("Console transports keep interleaved commands independent") {
  Platform platform;
  daveos::console::Input uart(platform), usb(platform);
  Feed(uart, "board ");
  Feed(usb, "help\n");
  Feed(uart, "stats\n");
  daveos::console::Line line;
  REQUIRE(usb.pop(line));
  CHECK(line.view() == "help");
  REQUIRE(uart.pop(line));
  CHECK(line.view() == "board stats");
}

TEST_CASE(
    "Chunk consumption stops at one line and retains partial CRLF state") {
  Platform platform;
  daveos::console::Input input(platform);
  daveos::console::Line line;
  std::string_view bytes = "one\r\ntwo\nthree\rfour";
  for (auto expected : {"one", "two", "three"}) {
    const auto result = input.consume(bytes, line);
    REQUIRE(result.complete);
    CHECK(line.view() == expected);
    bytes.remove_prefix(result.bytes);
  }
  auto result = input.consume(bytes, line);
  CHECK_FALSE(result.complete);
  CHECK(result.bytes == 4);
  result = input.consume(std::string_view(" more\r"), line);
  REQUIRE(result.complete);
  CHECK(line.view() == "four more");
  result = input.consume(std::string_view("\nfive\n"), line);
  REQUIRE(result.complete);
  CHECK(line.view() == "five");
  CHECK(result.bytes == 6);
}

TEST_CASE("Configurable input queue retains bounded multi-line bursts") {
  Platform platform;
  daveos::console::Input<Platform, 16> input(platform);
  // Sixteen maximum-size valid command lines, delivered before any polling.
  for (int i = 0; i < 16; ++i) {
    auto command = std::to_string(i);
    command.resize(256, ' ');
    Feed(input, command + "\r\n");
  }
  Feed(input, "discarded\n");
  CHECK(input.take_dropped() == 1);
  daveos::console::Line line;
  for (int i = 0; i < 16; ++i) {
    REQUIRE(input.pop(line));
    CHECK(line.size == 256);
    CHECK(line.view().starts_with(std::to_string(i) + " "));
  }
  CHECK_FALSE(input.pop(line));
  Feed(input, "recovered\n");
  REQUIRE(input.pop(line));
  CHECK(line.view() == "recovered");
  CHECK(input.take_dropped() == 0);
}

TEST_CASE("Terminal echo appends long input once and redraws edits safely") {
  terminal_output.clear();
  daveos::console::LineDisplay display(
      nullptr, [](void*, std::string_view text) { TerminalWrite(text); });
  daveos::console::Line line;
  for (std::size_t i = 0; i < 256; ++i) {
    line.bytes[i] = 'x';
    line.size = i + 1;
    display.show(line);
  }
  CHECK(terminal_output == std::string(256, 'x'));
  terminal_output.clear();
  line.size = 2;
  display.show(line);
  CHECK(terminal_output == "\r\x1b[2Kxx");
  terminal_output.clear();
  line.bytes[1] = '\x1b';
  display.show(line);
  CHECK(terminal_output == "\r\x1b[2Kx?");
  terminal_output.clear();
  line.bytes[2] = '\x7f';
  line.size = 3;
  display.show(line);
  CHECK(terminal_output == "?");
  terminal_output.clear();
  display.before_log();
  TerminalWrite("message\r\n");
  display.after_log();
  CHECK(terminal_output == "\r\x1b[2Kmessage\r\nx??");
}
