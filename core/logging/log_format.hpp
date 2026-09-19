#pragma once

#include <inttypes.h>

#include <cstdio>
#include <limits>

#include "core/logging/log.hpp"

namespace daveos::core {


// Display unsigned statistics without relying on embedded libc's 64-bit
// printf support. Values beyond uint32_t are shown as "4294967295+"; the
// underlying counter is unchanged. Storage remains valid for this object's
// life.
class LogUnsigned {
 public:
  explicit LogUnsigned(std::uint64_t value) {
    constexpr auto limit = std::numeric_limits<std::uint32_t>::max();
    std::snprintf(bytes_.data(), bytes_.size(), "%" PRIu32 "%s",
                  value > limit ? limit : static_cast<std::uint32_t>(value),
                  value > limit ? "+" : "");
  }

  const char* c_str() const { return bytes_.data(); }

 private:
  std::array<char, 12> bytes_{};
};

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
                      "[%03" PRIu32 ":%02" PRIu32 ":%02" PRIu32 ":%02" PRIu32
                      ".%03" PRIu32 "] %c %-*s: ",
                      static_cast<std::uint32_t>(hours / 24),
                      static_cast<std::uint32_t>(hours % 24),
                      static_cast<std::uint32_t>(minutes % 60),
                      static_cast<std::uint32_t>(seconds % 60),
                      static_cast<std::uint32_t>(milliseconds % 1000),
                      "DIWEF"[static_cast<std::size_t>(record.severity)],
                      static_cast<int>(ContextWidth), context.data());
    if (written > 0) size_ = static_cast<std::size_t>(written);
  }

  std::string_view view() const { return {bytes_.data(), size_}; }

 private:
  std::array<char, ContextWidth + 40> bytes_{};
  std::size_t size_ = 0;
};


}  // namespace daveos::core
