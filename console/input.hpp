#pragma once

#include <span>
#include <string_view>

#include "core/queue/queue.hpp"

namespace daveos::console {


  // Transport-side line collection, callable from an ISR or task; tokenization
  // and dispatch run as a task.
  // Keep an extra byte so the dispatcher can diagnose an overlength line.
  struct Line {
    std::array<char, 257> bytes{};
    std::size_t size = 0;

    std::string_view view() const { return {bytes.data(), size}; }
  };

  // Complete lines occupy fixed storage; overflow drops a whole new line.
  template <typename P, std::size_t LineCapacity = 4>
  class Input {
   public:
    explicit Input(P& platform) : platform_(platform) {}

    void receive(char byte) {
      daveos::core::Guard guard(platform_);
      Receive(byte);
    }

    struct Consumed {
      std::size_t bytes;
      bool complete;
    };

    // Consume at most one line, leaving bytes after its terminator to the
    // caller. Partial lines persist across calls, including a CRLF split
    // between chunks.
    Consumed consume(std::span<const char> bytes, Line& line) {
      daveos::core::Guard guard(platform_);
      if (lines_.pop(line) == daveos::core::Status::ok) {
        return {0, true};
      }
      for (std::size_t i = 0; i < bytes.size(); ++i) {
        Receive(bytes[i]);
        if (lines_.pop(line) == daveos::core::Status::ok) {
          return {i + 1, true};
        }
      }
      return {bytes.size(), false};
    }

    // A UART error invalidates the entire current line, through its terminator.
    void error() {
      daveos::core::Guard guard(platform_);
      if (!discard_) {
        ++dropped_;
      }
      discard_ = true;
    }

    // A transport session ended: do not combine old input with a new
    // connection.
    void reset() {
      daveos::core::Guard guard(platform_);
      lines_.clear();
      line_ = {};
      previous_cr_ = discard_ = false;
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
    void Receive(char byte) {
      if (byte == '\n' && previous_cr_) {
        previous_cr_ = false;
        return;
      }
      previous_cr_ = byte == '\r';
      if (byte == '\r' || byte == '\n') {
        if (!discard_ && lines_.push(line_) != daveos::core::Status::ok) {
          ++dropped_;
        }
        line_.size = 0;
        discard_ = false;
      } else if (byte == '\b' || byte == '\x7f') {
        // Once overlength, retain the rejection marker through Return.
        if (!discard_ && line_.size && line_.size < line_.bytes.size()) {
          --line_.size;
        }
      } else if (!discard_ && line_.size < line_.bytes.size()) {
        line_.bytes[line_.size++] = byte;
      }
    }

    P& platform_;
    daveos::core::Queue<Line, LineCapacity> lines_;
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
    LineDisplay(void* context, void (*write)(void*, std::string_view))
        : context_(context), write_(write) {}

    void show(const Line& line) {
      if (shown_.view() == line.view()) {
        return;
      }
      before_log();
      shown_ = line;
      after_log();
    }

    void clear() { show(Line{}); }

    // Forget presentation from a disconnected session without writing output.
    void reset() { shown_ = {}; }

    void before_log() {
      if (shown_.size) {
        write_(context_, "\r\x1b[2K");
      }
    }

    void after_log() {
      // Echo printable ASCII only; typed terminal escapes must not move the
      // cursor.
      if (!shown_.size) {
        return;
      }
      Line visible = shown_;
      for (std::size_t i = 0; i < visible.size; ++i) {
        auto& byte = visible.bytes[i];
        if (byte < ' ' || byte > '~') {
          byte = '?';
        }
      }
      write_(context_, visible.view());
    }

   private:
    void* context_;
    void (*write_)(void*, std::string_view);
    Line shown_;
  };


}  // namespace daveos::console
