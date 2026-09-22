#pragma once

#include <algorithm>
#include <string_view>

#include "core/state_machine/state_machine.hpp"
#include "hal/i2c/action.hpp"
#include "ssd1306_font.h"

namespace daveos::drivers {

  // SSD1306 128x64 I2C display, internal charge pump, no external reset GPIO.
  // Command sequence/font adapted from the Form fork of afiskon/stm32-ssd1306;
  // see ssd1306_LICENSE. Fixed framebuffer, asynchronous page writes, no heap.
  // One task owns all methods. tick() never waits. Keep this object and its
  // injected device alive until busy() is false; accepted transfers retain
  // their buffers until completion, including on timeout. No automatic retries.
  // Drawing is rejected while busy. A failed transfer requires initialize()
  // again (and controller reset first if the HAL reports a faulted bus).
  class Ssd1306 {
    using Status = hal::Status;

   public:
    static constexpr std::size_t kWidth = 128, kHeight = 64;

    explicit Ssd1306(const hal::i2c::Device& device) : device_(device) {}

    Status initialize() {
      if (busy_) {
        return Status::busy;
      }
      initialized_ = false;
      framebuffer_.fill(0);
      initialize_ = true;
      return Request();
    }

    Status refresh() {
      if (busy_) {
        return Status::busy;
      }
      if (!initialized_) {
        return Status::not_initialized;
      }
      initialize_ = false;
      return Request();
    }

    Status fill(bool white) {
      if (busy_) {
        return Status::busy;
      }
      framebuffer_.fill(white ? 0xff : 0);
      return Status::ok;
    }

    Status pixel(std::size_t x, std::size_t y, bool white = true) {
      if (busy_) {
        return Status::busy;
      }
      if (x >= kWidth || y >= kHeight) {
        return Status::invalid_argument;
      }
      auto& byte = framebuffer_[x + (y / 8) * kWidth];
      const auto mask = static_cast<std::uint8_t>(1u << (y % 8));
      byte = white ? byte | mask : byte & static_cast<std::uint8_t>(~mask);
      return Status::ok;
    }

    // One ASCII line in the 6x8 font. Reject the whole request if it won't fit.
    Status text(std::size_t x, std::size_t y, std::string_view text) {
      if (busy_) {
        return Status::busy;
      }
      if (x >= kWidth || y > kHeight - 8 || text.size() > (kWidth - x) / 6 ||
          std::any_of(text.begin(), text.end(),
                      [](unsigned char c) { return c < 32 || c > 126; })) {
        return Status::invalid_argument;
      }
      for (unsigned char c : text) {
        for (std::size_t row = 0; row < 8; ++row) {
          const auto bits = detail::kFont6x8[(c - 32) * 8 + row];
          for (std::size_t col = 0; col < 6; ++col) {
            (void)pixel(x + col, y + row, (bits & (0x8000u >> col)) != 0);
          }
        }
        x += 6;
      }
      return Status::ok;
    }

    void tick() { machine_.tick(*this); }

    bool busy() const { return busy_; }

    bool initialized() const { return initialized_; }

    std::optional<Status> result() const { return result_; }

   private:
    static constexpr auto kTimeout = std::chrono::milliseconds{100};
    // Page addressing, 64 COM lines, internal charge pump. Keep display off
    // until every cleared page has reached the controller.
    static constexpr std::array<std::uint8_t, 25> kInitialize{
        0x00, 0xae, 0x20, 0x02, 0xc8, 0x40, 0x81, 0xff, 0xa1,
        0xa6, 0xa8, 0x3f, 0xa4, 0xd3, 0x00, 0xd5, 0xf0, 0xd9,
        0x22, 0xda, 0x12, 0xdb, 0x20, 0x8d, 0x14};
    enum class State : std::uint8_t {
      idle,
      configuring,
      addressing,
      writing,
      enabling
    };

    class Machine : public core::StateMachine<Machine, State, State::idle, 5> {
     public:
      void Step(State cs, State& ns, Ssd1306& d) {
        switch (cs) {
          case State::idle:
            if (d.requested_) {
              d.requested_ = false;
              d.page_ = 0;
              if (d.initialize_) {
                d.Start(kInitialize);
                ns = State::configuring;
              } else {
                d.Address();
                ns = State::addressing;
              }
            }
            break;
          case State::configuring:
          case State::addressing:
          case State::writing:
          case State::enabling:
            if (const auto result = d.completion_.result()) {
              if (result->status != Status::ok) {
                d.Finish(result->status);
                ns = State::idle;
              } else if (cs == State::configuring) {
                d.Address();
                ns = State::addressing;
              } else if (cs == State::addressing) {
                d.packet_[0] = 0x40;
                std::copy_n(d.framebuffer_.begin() + d.page_ * kWidth, kWidth,
                            d.packet_.begin() + 1);
                d.Start(d.packet_);
                ns = State::writing;
              } else if (cs == State::writing && ++d.page_ < kHeight / 8) {
                d.Address();
                ns = State::addressing;
              } else if (cs == State::writing && d.initialize_) {
                d.command_[0] = 0;
                d.command_[1] = 0xaf;
                d.Start(std::span{d.command_}.first(2));
                ns = State::enabling;
              } else {
                d.initialized_ = true;
                d.Finish(Status::ok);
                ns = State::idle;
              }
            }
            break;
        }
      }
    };

    Status Request() {
      result_.reset();
      requested_ = busy_ = true;
      return Status::ok;
    }

    void Start(std::span<const std::uint8_t> bytes) {
      actions_[0] = hal::i2c::write(bytes);
      (void)completion_.start(device_, actions_, kTimeout);
    }

    void Address() {
      command_ = {0x00, static_cast<std::uint8_t>(0xb0 + page_), 0x00, 0x10};
      Start(command_);
    }

    void Finish(Status status) {
      result_ = status;
      busy_ = false;
      if (status != Status::ok) {
        initialized_ = false;
      }
    }

    hal::i2c::Device device_;
    hal::i2c::Completion completion_;
    Machine machine_;
    std::array<std::uint8_t, kWidth * kHeight / 8> framebuffer_{};
    std::array<std::uint8_t, kWidth + 1> packet_{};
    std::array<std::uint8_t, 4> command_{};
    std::array<hal::i2c::Action, 1> actions_{};
    std::optional<Status> result_;
    std::size_t page_ = 0;
    bool initialized_ = false, initialize_ = false, requested_ = false,
         busy_ = false;
  };
}  // namespace daveos::drivers
