#pragma once

#include <optional>

#include "core/state_machine/state_machine.hpp"
#include "hal/status.h"

namespace daveos::hal::i2c {


  // Board-injected, open-drain GPIO operations. true releases a line; false
  // drives it low. enter switches from disabled peripheral to released GPIO;
  // leave releases both lines and restores their alternate function. No hook
  // waits, drives high, or invokes application callbacks.
  struct BusClearPins {
    void* context = nullptr;
    void (*enter)(void*) = nullptr;
    void (*leave)(void*) = nullptr;
    void (*scl)(void*, bool) = nullptr;
    void (*sda)(void*, bool) = nullptr;
    bool (*scl_high)(void*) = nullptr;
    bool (*sda_high)(void*) = nullptr;

    constexpr bool valid() const {
      return enter && leave && scl && sda && scl_high && sda_high;
    }
  };

  // Single-controller bus clear: up to nine clocks, then STOP. Driven by the
  // controller's reset timer, with >=5 us low/high/setup/bus-free intervals.
  // The owner enforces a total deadline and must abort on expiry/timer failure.
  // A low SCL is never forced high: wait for release or that deadline.
  class BusClear {
   public:
    explicit constexpr BusClear(const BusClearPins& pins = {}) : pins_(pins) {}

    bool available() const { return pins_.valid(); }

    void start() {
      requested_ = true;
      result_.reset();
    }

    std::optional<Status> poll(std::uint64_t now) {
      machine_.tick(*this, now);
      return result_;
    }

    // Bounded immediate pin release. One tick takes any active waveform
    // state back to idle; the state machine remains reusable after abort.
    void abort() {
      abort_ = true;
      machine_.tick(*this, 0);
      abort_ = false;
    }

   private:
    static constexpr std::uint64_t kEdgeUs = 5;
    enum class State {
      idle,
      initial,
      low,
      rising,
      high,
      stop_low,
      stop_rising,
      stop_high,
      check,
      count
    };

    class Machine
        : public core::StateMachine<Machine, State, State::idle,
                                    static_cast<std::size_t>(State::count)> {
     public:
      void Step(State cs, State& ns, BusClear& b, std::uint64_t now) {
        switch (cs) {
          case State::idle:
            if (b.abort_) {
              b.requested_ = false;
              b.result_ = Status::timeout;
            } else if (b.requested_) {
              b.requested_ = false;
              b.pulses_ = 0;
              if (!b.available()) {
                b.result_ = Status::faulted;
              } else {
                b.pins_.enter(b.pins_.context);
                b.edge_ = now;
                ns = State::initial;
              }
            }
            break;
          case State::initial:
          case State::low:
          case State::rising:
          case State::high:
          case State::stop_low:
          case State::stop_rising:
          case State::stop_high:
          case State::check:
            if (b.abort_) {
              b.Finish(Status::timeout);
              ns = State::idle;
              break;
            }
            if (now - b.edge_ < kEdgeUs) {
              break;
            }
            switch (cs) {
              case State::initial:
                if (b.SclHigh()) {
                  if (b.SdaHigh()) {
                    b.Finish(Status::ok);
                    ns = State::idle;
                  } else {
                    b.Scl(false, now);
                    ns = State::low;
                  }
                }
                break;
              case State::low:
                b.Scl(true, now);
                ns = State::rising;
                break;
              case State::rising:
                if (b.SclHigh()) {
                  b.edge_ = now;
                  ns = State::high;
                }
                break;
              case State::high: {
                if (!b.SclHigh()) {
                  b.edge_ = now;
                  ns = State::rising;
                  break;
                }
                const bool released = b.SdaHigh();
                ++b.pulses_;
                b.Scl(false, now);
                if (released || b.pulses_ == 9) {
                  b.pins_.sda(b.pins_.context, false);
                  ns = State::stop_low;
                } else {
                  ns = State::low;
                }
                break;
              }
              case State::stop_low:
                b.Scl(true, now);
                ns = State::stop_rising;
                break;
              case State::stop_rising:
                if (b.SclHigh()) {
                  b.edge_ = now;
                  ns = State::stop_high;
                }
                break;
              case State::stop_high:
                if (!b.SclHigh()) {
                  b.edge_ = now;
                  ns = State::stop_rising;
                  break;
                }
                b.pins_.sda(b.pins_.context, true);
                b.edge_ = now;
                ns = State::check;
                break;
              case State::check:
                b.Finish(b.SclHigh() && b.SdaHigh() ? Status::ok
                                                    : Status::faulted);
                ns = State::idle;
                break;
              default:
                break;
            }
            break;
          case State::count:
            break;
        }
      }
    };

    bool SclHigh() const { return pins_.scl_high(pins_.context); }

    bool SdaHigh() const { return pins_.sda_high(pins_.context); }

    void Scl(bool release, std::uint64_t now) {
      pins_.scl(pins_.context, release);
      edge_ = now;
    }

    void Finish(Status status) {
      pins_.leave(pins_.context);
      result_ = status;
    }

    BusClearPins pins_;
    Machine machine_;
    std::optional<Status> result_;
    std::uint64_t edge_ = 0;
    std::uint8_t pulses_ = 0;
    bool requested_ = false, abort_ = false;
  };


}  // namespace daveos::hal::i2c
