#pragma once

#include "drivers/ssd1306.h"
#include "i2c_probe.hpp"

namespace app {

  // Optional demo. Commands submit work; the task sends one page at a time.
  // Initialize explicitly after i2c scan confirms the display address.
  class Display : public core::Module<Display, Event> {
   public:
    explicit Display(I2cBus& bus) : bus_(bus), panel_(I2cBus::device<1>(bus)) {}

    static constexpr const char* name() { return "display"; }

    static constexpr auto tasks() {
      return std::array{
          DAVEOS_PERIODIC(Display, Tick, std::chrono::milliseconds{1})};
    }

    static constexpr auto commands() {
      return std::array{
          DAVEOS_COMMAND(Display, Init, "init",
                         "Initialize 128x64 SSD1306 at 0x3c"),
          DAVEOS_COMMAND(Display, Text, "text",
                         "Show one ASCII line (up to 21 characters)",
                         core::arg("text")),
          DAVEOS_COMMAND(Display, Pattern, "pattern",
                         "Show border and checkerboard test pattern"),
          DAVEOS_COMMAND(Display, Clear, "clear", "Clear display")};
    }

    core::Status Init() {
      if (!Acquire()) {
        return core::Status::busy;
      }
      return Submit(panel_.initialize());
    }

    core::Status Text(std::string_view text) {
      if (text.size() > 21 ||
          std::any_of(text.begin(), text.end(),
                      [](unsigned char c) { return c < 32 || c > 126; })) {
        return core::Status::invalid_argument;
      }
      if (!Acquire()) {
        return core::Status::busy;
      }
      (void)panel_.fill(false);
      (void)panel_.text(0, 0, text);
      return Submit(panel_.refresh());
    }

    core::Status Pattern() {
      if (!Acquire()) {
        return core::Status::busy;
      }
      (void)panel_.fill(false);
      for (std::size_t y = 0; y < 64; ++y) {
        for (std::size_t x = 0; x < 128; ++x) {
          const bool white =
              !x || !y || x == 127 || y == 63 || ((x / 8 + y / 8) % 2 == 0);
          (void)panel_.pixel(x, y, white);
        }
      }
      return Submit(panel_.refresh());
    }

    core::Status Clear() {
      if (!Acquire()) {
        return core::Status::busy;
      }
      (void)panel_.fill(false);
      return Submit(panel_.refresh());
    }

    void Tick() {
      panel_.tick();
      if (pending_ && panel_.result()) {
        pending_ = false;
        bus_.release();
        if (*panel_.result() == daveos::hal::Status::ok) {
          I_("SSD1306 update complete");
        } else {
          E_("SSD1306: %s", enum_name(*panel_.result()));
        }
      }
    }

   private:
    bool Acquire() { return !pending_ && bus_.acquire(); }

    core::Status Submit(daveos::hal::Status status) {
      if (status != daveos::hal::Status::ok) {
        bus_.release();
        return core::Status::not_running;
      }
      pending_ = true;
      return core::Status::ok;
    }

    I2cBus& bus_;
    daveos::drivers::Ssd1306 panel_;
    bool pending_ = false;
  };
}  // namespace app
