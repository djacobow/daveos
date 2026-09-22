#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

#include "core/state_machine/state_machine.hpp"
#include "hal/spi/action.hpp"
#include "protocol.h"

namespace daveos::storage::sd {


  // Nonblocking SPI initialization for SDHC/SDXC cards. Configure mode 0,
  // 8-bit MSB-first transfers and SCK <=400 kHz before request(); keep that
  // configuration until result(). The caller owns power-up settling and bus
  // exclusion. No scheduler, logging, board or filesystem dependency.
  // Call request/tick/result on one execution thread. Keep this object and
  // its injected device alive and stationary until completion; callbacks only
  // publish completion. No cancellation, legacy SDSC support or error retries.
  class Initializer {
   public:
    struct Result {
      hal::Status status;
      std::uint32_t ocr;
      std::uint32_t command;
    };

    explicit Initializer(const hal::spi::Device& device) : device_(device) {}

    hal::Status request() {
      if (active_) {
        return hal::Status::busy;
      }
      result_.reset();
      requested_ = active_ = true;
      return hal::Status::ok;
    }

    void tick() { machine_.tick(*this); }

    std::optional<Result> result() const { return result_; }

    bool busy() const { return active_; }

   private:
    static constexpr auto kTransferTimeout = std::chrono::milliseconds{250};
    // Bound ACMD41 idle responses, preserving the existing probe policy.
    // Total initialization time also depends on the caller's tick cadence.
    static constexpr std::uint32_t kStartupLimit = 1000;
    enum class State { idle, clocks, command, gap, done, failed, count };

    struct Machine
        : core::StateMachine<Machine, State, State::idle,
                             static_cast<std::size_t>(State::count)> {
      void Step(State cs, State& ns, Initializer& p) {
        switch (cs) {
          case State::idle:
          case State::done:
          case State::failed:
            if (p.requested_) {
              p.requested_ = false;
              p.command_ = p.attempts_ = p.ocr_ = 0;
              p.status_ = hal::Status::response_mismatch;
              ns = State::clocks;
            }
            break;
          case State::clocks:
          case State::gap:
            if (const auto result = p.completion_.result()) {
              if (result->status == hal::Status::ok) {
                ns = State::command;
              } else {
                p.status_ = result->status;
                ns = State::failed;
              }
            }
            break;
          case State::command:
            if (const auto result = p.completion_.result()) {
              if (result->status != hal::Status::ok) {
                p.status_ = result->status;
                ns = State::failed;
                break;
              }
              std::size_t r = 0;
              while (r < 8 && p.rx_[r] == 0xff) {
                ++r;
              }
              if (r == 8) {
                ns = State::failed;
                break;
              }
              const auto response = p.rx_[r];
              ns = State::gap;
              switch (p.command_) {
                case 0:
                  if (response == 1) {
                    p.command_ = 8;
                  } else {
                    ns = State::failed;
                  }
                  break;
                case 8:
                  if (response == 1 && r + 4 < p.rx_.size() &&
                      p.rx_[r + 3] == 1 && p.rx_[r + 4] == 0xaa) {
                    p.command_ = 55;
                  } else {
                    ns = State::failed;
                  }
                  break;
                case 55:
                  if (response <= 1) {
                    p.command_ = 41;
                  } else {
                    ns = State::failed;
                  }
                  break;
                case 41:
                  if (response == 0) {
                    p.command_ = 58;
                  } else if (response == 1 && ++p.attempts_ < kStartupLimit) {
                    p.command_ = 55;
                  } else {
                    ns = State::failed;
                  }
                  break;
                case 58:
                  if (response == 0 && r + 4 < p.rx_.size()) {
                    p.ocr_ = (std::uint32_t{p.rx_[r + 1]} << 24) |
                             (std::uint32_t{p.rx_[r + 2]} << 16) |
                             (std::uint32_t{p.rx_[r + 3]} << 8) | p.rx_[r + 4];
                    if ((p.ocr_ & 0xc0000000) != 0xc0000000) {
                      ns = State::failed;
                    } else {
                      ns = State::done;
                    }
                  } else {
                    ns = State::failed;
                  }
                  break;
                default:
                  ns = State::failed;
                  break;
              }
            }
            break;
          case State::count:
            break;
        }
      }

      void OnEnter(State state, Initializer& p) {
        if (state == State::clocks || state == State::gap) {
          p.actions_[0] =
              hal::spi::idle_clocks(state == State::clocks ? 80 : 8);
          (void)p.completion_.start(p.device_, std::span{p.actions_}.first(1),
                                    kTransferTimeout);
        } else if (state == State::command) {
          p.tx_ = command(p.command_, p.command_ == 8    ? 0x1aa
                                      : p.command_ == 41 ? 0x40000000
                                                         : 0);
          p.rx_.fill(0xff);
          p.actions_ = {hal::spi::write(p.tx_), hal::spi::read(p.rx_)};
          (void)p.completion_.start(p.device_, p.actions_, kTransferTimeout);
        } else if (state == State::done || state == State::failed) {
          p.result_ = Result{state == State::done ? hal::Status::ok : p.status_,
                             p.ocr_, p.command_};
          p.active_ = false;
        }
      }
    };

    hal::spi::Device device_;
    hal::spi::Completion completion_;
    Machine machine_;
    std::array<std::uint8_t, 6> tx_{};
    std::array<std::uint8_t, 16> rx_{};
    std::array<hal::spi::Action, 2> actions_{};
    std::optional<Result> result_;
    hal::Status status_ = hal::Status::ok;
    std::uint32_t command_ = 0, attempts_ = 0, ocr_ = 0;
    bool requested_ = false, active_ = false;
  };


}  // namespace daveos::storage::sd
