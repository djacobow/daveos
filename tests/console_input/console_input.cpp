#include <string>
#include <string_view>

#include "../../examples/stm32_console/input.hpp"
#include "catch_amalgamated.hpp"
#include "platform/fake/platform.h"

namespace {
using Platform = daveos::platform::fake::Platform;
void Feed(app::Input<Platform>& input, std::string_view bytes) {
  for (auto byte : bytes) input.receive(byte);
}
std::string_view View(const app::Line& line) {
  return {line.bytes.data(), line.size};
}
}  // namespace
TEST_CASE("UART lines accept CR, LF and CRLF without duplicate commands") {
  Platform platform;
  app::Input input(platform);
  Feed(input, "one\rtwo\nthree\r\nfour");
  app::Line line;
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
  app::Input input(platform);
  for (int i = 0; i < 400; ++i) input.receive('x');
  Feed(input, "\nhelp\n");
  app::Line line;
  REQUIRE(input.pop(line));
  CHECK(line.size == 257);
  REQUIRE(input.pop(line));
  CHECK(View(line) == "help");
  CHECK_FALSE(input.pop(line));
}
TEST_CASE("UART queue overflow drops whole lines and recovers") {
  Platform platform;
  app::Input input(platform);
  Feed(input, "a\nb\nc\nd\nbad\n");
  CHECK(input.take_dropped() == 1);
  CHECK(input.take_dropped() == 0);
  app::Line line;
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
  app::Input input(platform);
  Feed(input, "board led ");
  input.error();
  input.error();
  Feed(input, "1 on\r\nhelp\n");
  app::Line line;
  REQUIRE(input.pop(line));
  CHECK(View(line) == "help");
  CHECK_FALSE(input.pop(line));
  CHECK(input.take_dropped() == 1);
}

TEST_CASE("UART preview tracks editing and clears after Return or error") {
  Platform platform;
  app::Input input(platform);
  Feed(input,
       "helx\bp\x7f"
       "p");
  CHECK(input.preview().view() == "help");
  Feed(input, "\r\n");
  CHECK(input.preview().view().empty());
  app::Line line;
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
  app::LineDisplay display(TerminalWrite);
  app::Line line;
  line.bytes[0] = 'h';
  line.size = 1;
  display.show(line);
  CHECK(terminal_output == "h");
  display.show(line);
  CHECK(terminal_output == "h");
  line.bytes[1] = 'i';
  line.size = 2;
  display.show(line);
  CHECK(terminal_output == "h\r\x1b[2Khi");
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
