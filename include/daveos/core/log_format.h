#pragma once

#include <cstdio>

#include "daveos/core/log.h"

namespace daveos::core {


// Optional subscriber-side presentation; buffered records retain full names and
// microsecond timestamps. Days have at least three digits, milliseconds
// truncate sub-millisecond time. Context is left aligned and ellipsized to
// ContextWidth. The returned view borrows this object's fixed storage. No line
// ending included.
template <std::size_t ContextWidth = 22>
class LogPrefix {
  static_assert(ContextWidth >= 3 && ContextWidth <= 256);

 public:
  explicit LogPrefix(const LogRecord& record) {
    std::array<char, ContextWidth + 1> context{};
    auto length = std::snprintf(context.data(), context.size(), "%s.%s",
                                record.module, record.task);
    if (length > static_cast<int>(ContextWidth)) {
      for (auto i = ContextWidth - 3; i < ContextWidth; ++i) context[i] = '.';
    }
    auto milliseconds = record.timestamp / 1000;
    auto seconds = milliseconds / 1000;
    auto minutes = seconds / 60;
    auto hours = minutes / 60;
    // Even the largest uint64 microsecond timestamp fits in 32-bit days.
    auto written =
        std::snprintf(bytes_.data(), bytes_.size(),
                      "[%03lu:%02lu:%02lu:%02lu.%03lu] %c %-*s: ",
                      static_cast<unsigned long>(hours / 24),
                      static_cast<unsigned long>(hours % 24),
                      static_cast<unsigned long>(minutes % 60),
                      static_cast<unsigned long>(seconds % 60),
                      static_cast<unsigned long>(milliseconds % 1000),
                      "DIWEF"[static_cast<unsigned>(record.severity)],
                      static_cast<int>(ContextWidth), context.data());
    if (written > 0) size_ = static_cast<std::size_t>(written);
  }
  std::string_view view() const { return {bytes_.data(), size_}; }

 private:
  std::array<char, ContextWidth + 40> bytes_{};
  std::size_t size_ = 0;
};


}  // namespace daveos::core
