#pragma once

#include <string_view>

#include "daveos/core/queue.h"

namespace app {


// ISR-side line collection only; tokenization and dispatch run as a task.
// Keep an extra byte so the dispatcher can diagnose an overlength line.
struct Line {
  std::array<char, 257> bytes{};
  std::size_t size = 0;
  std::string_view view() const { return {bytes.data(), size}; }
};
template <typename P>
class Input {
 public:
  explicit Input(P& platform) : platform_(platform) {}
  void receive(char byte) {
    daveos::core::Guard guard(platform_);
    if (byte == '\n' && previous_cr_) {
      previous_cr_ = false;
      return;
    }
    previous_cr_ = byte == '\r';
    if (byte == '\r' || byte == '\n') {
      if (!discard_ && lines_.push(line_) != daveos::core::Status::ok)
        ++dropped_;
      line_.size = 0;
      discard_ = false;
    } else if (byte == '\b' || byte == '\x7f') {
      // Once overlength, retain the rejection marker through Return.
      if (!discard_ && line_.size && line_.size < line_.bytes.size())
        --line_.size;
    } else if (!discard_ && line_.size < line_.bytes.size()) {
      line_.bytes[line_.size++] = byte;
    }
  }
  // A UART error invalidates the entire current line, through its terminator.
  void error() {
    daveos::core::Guard guard(platform_);
    if (!discard_) ++dropped_;
    discard_ = true;
  }
  bool pop(Line& line) {
    daveos::core::Guard guard(platform_);
    return lines_.pop(line) == daveos::core::Status::ok;
  }
  // A coherent snapshot for task-side echo; never write the UART in an ISR.
  Line preview() {
    daveos::core::Guard guard(platform_);
    return discard_ ? Line{} : line_;
  }
  std::uint32_t take_dropped() {
    daveos::core::Guard guard(platform_);
    return std::exchange(dropped_, 0);
  }

 private:
  P& platform_;
  daveos::core::Queue<Line, 4> lines_;
  Line line_;
  bool previous_cr_ = false, discard_ = false;
  std::uint32_t dropped_ = 0;
};


// Single-threaded terminal presentation. Erase/redraw around asynchronous log
// output so that an unfinished command stays visible below the log records.
// Requires an ANSI terminal; keep input within its width (no wrapped-line
// editor).
class LineDisplay {
 public:
  explicit LineDisplay(void (*write)(std::string_view)) : write_(write) {}
  void show(const Line& line) {
    if (shown_.view() == line.view()) return;
    before_log();
    shown_ = line;
    after_log();
  }
  void clear() { show(Line{}); }
  void before_log() {
    if (shown_.size) write_("\r\x1b[2K");
  }
  void after_log() {
    // Echo printable ASCII only; typed terminal escapes must not move the
    // cursor.
    if (!shown_.size) return;
    Line visible = shown_;
    for (std::size_t i = 0; i < visible.size; ++i) {
      auto& byte = visible.bytes[i];
      if (byte < ' ' || byte > '~') byte = '?';
    }
    write_(visible.view());
  }

 private:
  void (*write_)(std::string_view);
  Line shown_;
};


}  // namespace app
