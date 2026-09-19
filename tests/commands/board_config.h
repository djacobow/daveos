#pragma once

#include <array>

#include "platform/fake/platform.h"

namespace board {


  using Platform = daveos::platform::fake::Platform;
  inline constexpr const char* kName = "test board";
  inline std::array<bool, 3> leds{};
  inline std::size_t writes = 0;

  inline void ToggleLed(std::size_t index) {
    leds.at(index) = !leds.at(index);
    ++writes;
  }

  inline void SetLed(std::size_t index, bool value) {
    leds.at(index) = value;
    ++writes;
  }

  inline bool ReadButton() { return false; }


}  // namespace board
