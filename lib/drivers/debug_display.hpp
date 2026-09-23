#pragma once

#include <array>
#include <cinttypes>
#include <cstdio>
#include <span>

#include "core/schedule/module.hpp"
#include "util/version.h"

namespace daveos::drivers {


  inline constexpr std::size_t kDisplayColumns = 21;
  using DisplayLine = std::array<char, kDisplayColumns + 1>;
  using DisplayLines = std::array<DisplayLine, 8>;

  // Called synchronously once per application row (0..4), then row 5 for
  // serial-number text. Write at most output.size() bytes; the module always
  // terminates, clips and sanitizes the result. Do not retain the span.
  // Serial text is prefixed with "SN:" by the module. Empty means unavailable.
  struct DisplayContent {
    void* context = nullptr;
    void (*line)(void*, std::size_t, std::span<char>) = nullptr;
  };

  // A 1 Hz status renderer, separate from the target's fast transfer task.
  // Target supplies live() and show(DisplayLines): show copies/consumes all
  // rows before returning and never retains the borrowed frame. Busy frames
  // are skipped, not queued; accepted frames complete in the target's task.
  // Platform, target and callback context outlive this module. Constructors
  // are passive; callbacks run only after both initialization stages complete.
  template <typename Platform, typename Target, typename Event = core::NoEvent>
  class DebugDisplay
      : public core::Module<DebugDisplay<Platform, Target, Event>, Event> {
   public:
    DebugDisplay(Platform& platform, Target& target,
                 const util::Version& version, DisplayContent content = {})
        : platform_(platform),
          target_(target),
          version_(version),
          content_(content) {}

    static constexpr const char* name() { return "debug_display"; }

    static constexpr auto tasks() {
      return std::array{
          DAVEOS_PERIODIC(DebugDisplay, Update, std::chrono::seconds{1})};
    }

    void Update() {
      if (!target_.live()) {
        return;
      }
      DisplayLines frame{};
      const auto seconds = platform_.now() / 1000000;
      std::snprintf(frame[0].data(), frame[0].size(),
                    "Up:%" PRIu32 "d %02" PRIu32 ":%02" PRIu32 ":%02" PRIu32,
                    static_cast<std::uint32_t>(seconds / 86400),
                    static_cast<std::uint32_t>((seconds / 3600) % 24),
                    static_cast<std::uint32_t>((seconds / 60) % 60),
                    static_cast<std::uint32_t>(seconds % 60));
      for (std::size_t row = 0; row < 5; ++row) {
        Fill(row, frame[row + 1]);
      }
      DisplayLine serial{};
      Fill(5, serial);
      std::snprintf(frame[6].data(), frame[6].size(), "SN:%.18s",
                    serial[0] ? serial.data() : "(none)");
      if (version_.build == util::Version::kLocal) {
        std::snprintf(frame[7].data(), frame[7].size(),
                      "FW:%" PRIu32 ".%" PRIu32 " local", version_.major,
                      version_.minor);
      } else {
        std::snprintf(frame[7].data(), frame[7].size(),
                      "FW:%" PRIu32 ".%" PRIu32 ".%" PRIu32, version_.major,
                      version_.minor, version_.build);
      }
      (void)target_.show(frame);
    }

   private:
    void Fill(std::size_t row, DisplayLine& line) {
      if (content_.line) {
        content_.line(content_.context, row, line);
      }
      line.back() = '\0';
      for (auto& c : line) {
        if (!c) {
          break;
        }
        if (static_cast<unsigned char>(c) < 32 ||
            static_cast<unsigned char>(c) > 126) {
          c = '?';
        }
      }
    }

    Platform& platform_;
    Target& target_;
    util::Version version_;
    DisplayContent content_;
  };


}  // namespace daveos::drivers
