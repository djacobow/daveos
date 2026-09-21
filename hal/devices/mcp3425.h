#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "core/state_machine/state_machine.hpp"
#include "hal/i2c/action.hpp"

namespace daveos::hal {


  // Asynchronous MCP3425 one-shot reader: 16 bits, gain 1, no register address.
  // Call request()/tick()/result() from one task context. tick takes monotonic
  // microseconds; it never waits. The injected device and this nonmoving object
  // must outlive every accepted transaction, including after conversion
  // timeout.
  class Mcp3425 {
   public:
    struct Sample {
      Status status = Status::ok;
      std::int16_t code = 0;
      // 62.5 uV/LSB; integer microvolts truncate toward zero.
      std::int32_t microvolts = 0;
    };

    explicit Mcp3425(const i2c::Device& device) : device_(device) {}

    Status request() {
      if (active_) {
        return Status::busy;
      }
      result_.reset();
      active_ = requested_ = true;
      return Status::ok;
    }

    void tick(std::uint64_t now) { machine_.tick(*this, now); }

    std::optional<Sample> result() const { return result_; }

    bool busy() const { return active_; }

   private:
    static constexpr auto kTransferTimeout = std::chrono::milliseconds{10};
    static constexpr std::uint64_t kConversionTimeoutUs = 250000;
    static constexpr std::uint64_t kPollIntervalUs = 5000;
    static constexpr std::uint8_t kConfiguration = 0x88;

    enum class State : std::uint8_t { idle, writing, waiting, reading };

    class Machine : public core::StateMachine<Machine, State, State::idle, 4> {
     public:
      void Step(State cs, State& ns, Mcp3425& adc, std::uint64_t now) {
        switch (cs) {
          case State::idle:
            if (adc.requested_) {
              adc.requested_ = false;
              adc.started_ = now;
              adc.actions_[0] = i2c::write(adc.config_);
              (void)adc.completion_.start(adc.device_, adc.actions_,
                                          kTransferTimeout);
              ns = State::writing;
            }
            break;
          case State::writing:
            if (const auto result = adc.completion_.result()) {
              if (result->status != Status::ok) {
                adc.Finish(result->status);
                ns = State::idle;
              } else {
                adc.last_poll_ = now;
                ns = State::waiting;
              }
            }
            break;
          case State::waiting:
            if (now - adc.started_ >= kConversionTimeoutUs) {
              adc.Finish(Status::timeout);
              ns = State::idle;
            } else if (now - adc.last_poll_ >= kPollIntervalUs) {
              adc.actions_[0] = i2c::read(adc.bytes_);
              (void)adc.completion_.start(adc.device_, adc.actions_,
                                          kTransferTimeout);
              ns = State::reading;
            }
            break;
          case State::reading:
            // Never publish completion/reuse buffers until the HAL releases
            // ownership, even if the conversion deadline has already passed.
            if (const auto result = adc.completion_.result()) {
              if (result->status != Status::ok) {
                adc.Finish(result->status);
                ns = State::idle;
              } else if (now - adc.started_ >= kConversionTimeoutUs) {
                adc.Finish(Status::timeout);
                ns = State::idle;
              } else if ((adc.bytes_[2] & 0x7f) != (kConfiguration & 0x7f)) {
                adc.Finish(Status::response_mismatch);
                ns = State::idle;
              } else if (!(adc.bytes_[2] & 0x80)) {
                adc.Finish(Status::ok);
                ns = State::idle;
              } else {
                adc.last_poll_ = now;
                ns = State::waiting;
              }
            }
            break;
        }
      }
    };

    void Finish(Status status) {
      const std::int32_t raw = (std::uint32_t{bytes_[0]} << 8) | bytes_[1];
      const auto code =
          static_cast<std::int16_t>(raw >= 32768 ? raw - 65536 : raw);
      result_ = Sample{status, status == Status::ok ? code : std::int16_t{0},
                       status == Status::ok ? std::int32_t{code} * 125 / 2 : 0};
      active_ = false;
    }

    i2c::Device device_;
    i2c::Completion completion_;
    Machine machine_;
    std::array<std::uint8_t, 1> config_{kConfiguration};
    std::array<std::uint8_t, 3> bytes_{};
    std::array<i2c::Action, 1> actions_{};
    std::optional<Sample> result_;
    std::uint64_t started_ = 0, last_poll_ = 0;
    bool active_ = false, requested_ = false;
  };


}  // namespace daveos::hal
