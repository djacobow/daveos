#pragma once

#include "daveos/core/queue.h"

namespace app {


// ISR-side line collection only; tokenization and dispatch run as a task.
// Keep an extra byte so the dispatcher can diagnose an overlength line.
struct Line {
  std::array<char, 257> bytes{};
  std::size_t size = 0;
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


}  // namespace app
