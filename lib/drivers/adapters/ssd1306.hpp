#pragma once

#include "drivers/debug_display.hpp"
#include "drivers/ssd1306.h"

namespace daveos::drivers {

  // Optional SSD1306 module. Bus supplies task-time acquire()/release().
  // Bus/device outlive the module; do not destroy it during pending I/O.
  // Automatic failures silently disable live mode; manual requests log errors.
  // Commands submit work; the task sends one page at a time.
  // DebugDisplay submits automatic frames; manual commands pause that view.
  template <typename Bus, typename Event = core::NoEvent>
  class Ssd1306Module : public core::Module<Ssd1306Module<Bus, Event>, Event> {
   public:
    Ssd1306Module(Bus& bus, const hal::i2c::Device& device)
        : bus_(bus), panel_(device) {}

    static constexpr const char* name() { return "display"; }

    static constexpr auto tasks() {
      return std::array{
          DAVEOS_PERIODIC(Ssd1306Module, Tick, std::chrono::milliseconds{1})};
    }

    static constexpr auto commands() {
      return std::array{
          DAVEOS_COMMAND(Ssd1306Module, Init, "init",
                         "Initialize 128x64 SSD1306"),
          DAVEOS_COMMAND(Ssd1306Module, Text, "text",
                         "Show one ASCII line (up to 21 characters)",
                         core::arg("text")),
          DAVEOS_COMMAND(Ssd1306Module, Pattern, "pattern",
                         "Show border and checkerboard test pattern"),
          DAVEOS_COMMAND(Ssd1306Module, Clear, "clear", "Clear display"),
          DAVEOS_COMMAND(Ssd1306Module, Live, "live",
                         "Enable/disable automatic status display",
                         core::arg("enabled").friendly())};
    }

    bool live() const { return live_; }

    core::Status Live(bool enabled) {
      live_ = enabled;
      I_("Live display %s", enabled ? "on" : "off");
      return core::Status::ok;
    }

    // Consume the borrowed rows now; only the panel's owned framebuffer is
    // retained for asynchronous I2C. First automatic request initializes;
    // the next 1 Hz invocation draws a frame. Errors disable live updates.
    core::Status show(const daveos::drivers::DisplayLines& frame) {
      if (!Acquire()) {
        return core::Status::busy;
      }
      quiet_ = true;
      if (!panel_.initialized()) {
        return Submit(panel_.initialize());
      }
      (void)panel_.fill(false);
      for (std::size_t row = 0; row < frame.size(); ++row) {
        const auto end = std::find(frame[row].begin(), frame[row].end(), '\0');
        const std::string_view text(
            frame[row].data(),
            std::min<std::size_t>(kDisplayColumns, end - frame[row].begin()));
        const auto status = panel_.text(0, row * 8, text);
        if (status != hal::Status::ok) {
          bus_.release();
          return core::Status::invalid_argument;
        }
      }
      return Submit(panel_.refresh());
    }

    core::Status Init() {
      live_ = false;
      if (!Acquire()) {
        return core::Status::busy;
      }
      quiet_ = false;
      return Submit(panel_.initialize());
    }

    core::Status Text(std::string_view text) {
      live_ = false;
      if (text.size() > 21 ||
          std::any_of(text.begin(), text.end(),
                      [](unsigned char c) { return c < 32 || c > 126; })) {
        return core::Status::invalid_argument;
      }
      if (!Acquire()) {
        return core::Status::busy;
      }
      (void)panel_.fill(false);
      quiet_ = false;
      (void)panel_.text(0, 0, text);
      return Submit(panel_.refresh());
    }

    core::Status Pattern() {
      live_ = false;
      if (!Acquire()) {
        return core::Status::busy;
      }
      (void)panel_.fill(false);
      quiet_ = false;
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
      live_ = false;
      if (!Acquire()) {
        return core::Status::busy;
      }
      (void)panel_.fill(false);
      quiet_ = false;
      return Submit(panel_.refresh());
    }

    void Tick() {
      panel_.tick();
      if (pending_ && panel_.result()) {
        pending_ = false;
        bus_.release();
        if (*panel_.result() == daveos::hal::Status::ok) {
          if (!quiet_) {
            I_("SSD1306 update complete");
          }
        } else {
          live_ = false;
          if (!quiet_) {
            E_("SSD1306: %s", enum_name(*panel_.result()));
          }
        }
      }
    }

   private:
    bool Acquire() { return !pending_ && bus_.acquire(); }

    core::Status Submit(daveos::hal::Status status) {
      if (status != daveos::hal::Status::ok) {
        bus_.release();
        live_ = false;
        return core::Status::not_running;
      }
      pending_ = true;
      return core::Status::ok;
    }

    Bus& bus_;
    daveos::drivers::Ssd1306 panel_;
    bool pending_ = false, live_ = true, quiet_ = false;
  };
}  // namespace daveos::drivers
