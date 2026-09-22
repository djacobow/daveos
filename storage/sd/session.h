#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>

#include "core/state_machine/state_machine.hpp"
#include "hal/spi/action.hpp"
#include "initializer.h"
#include "protocol.h"
#include "read.h"
#include "storage/block_device.h"
#include "transport.h"

namespace daveos::storage::sd {


  // One SPI SD card's lifecycle over an injected bus: initialize, read the CSD,
  // expose the card as a BlockDevice, and recover through an explicit reset.
  //
  // Admission: request() and reset() return busy while a probe or reset runs,
  //   or while the block device is leased; neither starts work inline.
  // Ownership: the session owns its command/receive buffers. It borrows the
  //   SPI device and hooks, which must outlive it and stay stationary.
  // Execution: request(), reset() and tick() belong to one task context.
  //   Interrupts only publish HAL completions. Block reads/writes pump the
  //   injected yield while a transfer is pending.
  // Deadline: each transaction has a HAL timeout; expiry fails the probe.
  // Failure: a failed probe, read or write clears ready. Nothing retries;
  //   call request() again (after reset() if the bus is faulted).
  // Lifetime: the caller quiesces the bus before destroying the session.
  //
  // No logging: an optional Observer receives progress and failures. It may
  // also run further register/sector reads after the CSD (diagnostics).
  class Session {
    static constexpr std::size_t kSectorSize = 512;
    // R1, data token, one sector and its CRC; no fixed token-wait window.
    static constexpr std::size_t kReceiveCapacity = kSectorSize + 4;
    static constexpr auto kTransferTimeout = std::chrono::milliseconds{250};

   public:
    static constexpr std::uint32_t kStartupHz = 400000;
    static constexpr std::uint32_t kDataHz = Transport::kMaximumHz;

    // Requested SCK rate; set() takes effect for the next transaction and is
    // only called between transactions. rate() reports the actual rate.
    struct Speed {
      void* context = nullptr;
      void (*set)(void*, std::uint32_t hz) = nullptr;
      std::uint32_t (*rate)(void*) = nullptr;
    };

    // Start an asynchronous controller reset; the callback may run in
    // interrupt context. A rejected start is reported through the return.
    struct Reset {
      void* context = nullptr;
      hal::Status (*start)(void*, hal::Callback<hal::ResetResult>) = nullptr;
    };

    // Next step after a verified read. Reads are CMD17 (argument = LBA) or a
    // 16-byte register command such as CMD10.
    struct Step {
      enum class Kind { read, finish, fail } kind = Kind::finish;
      std::uint32_t command = 0;
      std::uint32_t argument = 0;
      const char* error = nullptr;
    };

    // All hooks are optional and run in the session's task context.
    struct Observer {
      void* context = nullptr;
      void (*started)(void*) = nullptr;
      // Called after the CSD (command 9) and every later read. Without it,
      // the session switches to its data rate and finishes after the CSD.
      Step (*next)(void*, std::uint32_t command,
                   std::span<const std::uint8_t> data) = nullptr;
      void (*completed)(void*, bool success) = nullptr;
      void (*reset_completed)(void*, hal::Status) = nullptr;
      void (*read_failed)(void*, std::uint32_t sector) = nullptr;
      void (*write_failed)(void*, std::uint32_t sector,
                           const Transport::WriteDiagnostics&) = nullptr;
    };

    // Construction only stores borrowed references.
    Session(const hal::spi::Device& device, const Speed& speed,
            const Reset& reset, const Transport::Pump& pump)
        : Session(device, speed, reset, pump, Observer{}) {}

    Session(const hal::spi::Device& device, const Speed& speed,
            const Reset& reset, const Transport::Pump& pump,
            const Observer& observer)
        : device_(device),
          speed_(speed),
          reset_(reset),
          pump_(pump),
          observer_(observer),
          initializer_(device) {}

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Initialize the card and read its CSD. Clears ready immediately.
    hal::Status request() {
      if (Busy()) {
        return hal::Status::busy;
      }
      ready_ = false;
      requested_ = true;
      return hal::Status::ok;
    }

    // Reset the SPI controller. The card stays not ready until request().
    hal::Status reset() {
      if (Busy()) {
        return hal::Status::busy;
      }
      ready_ = false;
      reset_requested_ = true;
      return hal::Status::ok;
    }

    void tick() { machine_.tick(*this); }

    bool ready() const { return ready_; }

    bool running() const { return running_ || requested_ || reset_requested_; }

    const Card& card() const { return card_; }

    std::uint32_t ocr() const { return ocr_; }

    // Command in progress, or the one that failed.
    std::uint32_t command() const { return command_; }

    const char* error() const { return error_; }

    std::span<const std::uint8_t> wire() const { return rx_; }

    std::optional<Initializer::Result> initialization() const {
      return initializer_.result();
    }

    std::optional<hal::spi::Result> last_transfer() const {
      return completion_.result();
    }

    // Borrowed view for FatFs or another block consumer; takes only this
    // address. acquire() fails unless the card is ready and idle.
    BlockDevice block_device() {
      return {this,
              [](void* p) { return static_cast<Session*>(p)->ready_; },
              [](void* p) { return static_cast<Session*>(p)->card_.sectors; },
              [](void* p, std::uint32_t sector, std::span<std::uint8_t> bytes) {
                auto& self = *static_cast<Session*>(p);
                if (!self.ready_ || !self.leased_) {
                  return false;
                }
                auto reader = self.Transfer();
                if (!reader.read(sector, bytes)) {
                  self.error_ = enum_name(reader.status());
                  self.ready_ = false;
                  if (self.observer_.read_failed) {
                    self.observer_.read_failed(self.observer_.context, sector);
                  }
                  return false;
                }
                return true;
              },
              [](void* p) {
                auto& self = *static_cast<Session*>(p);
                if (!self.ready_ || self.leased_ || self.running_ ||
                    self.requested_) {
                  return false;
                }
                self.leased_ = true;
                return true;
              },
              [](void* p) { static_cast<Session*>(p)->leased_ = false; },
              [](void* p, std::uint32_t sector,
                 std::span<const std::uint8_t> bytes) {
                auto& self = *static_cast<Session*>(p);
                if (!self.ready_ || !self.leased_) {
                  return false;
                }
                auto transport = self.Transfer();
                if (!transport.write(sector, bytes)) {
                  self.error_ = enum_name(transport.status());
                  self.ready_ = false;
                  if (self.observer_.write_failed) {
                    self.observer_.write_failed(self.observer_.context, sector,
                                                transport.write_diagnostics());
                  }
                  return false;
                }
                return true;
              },
              [](void* p) {
                const auto& self = *static_cast<Session*>(p);
                // Every successful sector write waits for ready and CMD13.
                return self.ready_ && self.leased_;
              }};
    }

   private:
    enum class State {
      idle,
      resetting,
      initializing,
      command,
      gap,
      done,
      failed,
      count
    };

    struct Machine
        : core::StateMachine<Machine, State, State::idle,
                             static_cast<std::size_t>(State::count)> {
      void Step(State cs, State& ns, Session& s) {
        switch (cs) {
          case State::idle:
          case State::done:
          case State::failed:
            if (s.reset_requested_) {
              s.reset_requested_ = false;
              s.running_ = true;
              ns = State::resetting;
            } else if (s.requested_) {
              s.requested_ = false;
              s.running_ = true;
              s.SetSpeed(kStartupHz);
              s.command_ = 0;
              s.argument_ = 0;
              s.error_ = "invalid command response";
              if (s.observer_.started) {
                s.observer_.started(s.observer_.context);
              }
              (void)s.initializer_.request();
              ns = State::initializing;
            }
            break;
          case State::resetting:
            if (s.reset_ready_.load(std::memory_order_acquire)) {
              if (s.reset_status_ != hal::Status::ok) {
                s.error_ = enum_name(s.reset_status_);
              }
              s.running_ = false;
              if (s.observer_.reset_completed) {
                s.observer_.reset_completed(s.observer_.context,
                                            s.reset_status_);
              }
              // Reset only repairs SPI; card readiness remains false.
              ns = State::idle;
            }
            break;
          case State::initializing:
            s.initializer_.tick();
            if (const auto result = s.initializer_.result()) {
              s.ocr_ = result->ocr;
              s.command_ = result->command;
              if (result->status == hal::Status::ok) {
                s.command_ = 9;
                ns = State::gap;
              } else {
                s.error_ = enum_name(result->status);
                ns = State::failed;
              }
            }
            break;
          case State::gap:
            if (auto result = s.completion_.result()) {
              if (result->status == hal::Status::ok) {
                ns = State::command;
              } else {
                s.error_ = enum_name(result->status);
                ns = State::failed;
              }
            }
            break;
          case State::command:
            if (auto result = s.completion_.result()) {
              if (result->status != hal::Status::ok) {
                s.error_ = enum_name(result->status);
                ns = State::failed;
                break;
              }
              switch (s.Received()) {
                case Step::Kind::read:
                  ns = State::gap;
                  break;
                case Step::Kind::finish:
                  ns = State::done;
                  break;
                case Step::Kind::fail:
                  ns = State::failed;
                  break;
              }
            }
            break;
          case State::count:
            break;
        }
      }

      void OnEnter(State state, Session& s) {
        if (state == State::resetting) {
          s.reset_ready_.store(false, std::memory_order_release);
          const auto status =
              s.reset_.start
                  ? s.reset_.start(s.reset_.context,
                                   hal::Callback<hal::ResetResult>::bind<
                                       &Session::ResetDone>(s))
                  : hal::Status::invalid_argument;
          if (status != hal::Status::ok) {
            s.ResetDone({status});
          }
        } else if (state == State::gap) {
          s.actions_[0] = hal::spi::idle_clocks(8);
          (void)s.completion_.start(s.device_, std::span{s.actions_}.first(1),
                                    kTransferTimeout);
        } else if (state == State::command) {
          s.tx_ = sd::command(s.command_, s.argument_);
          s.rx_.fill(0xff);
          s.receive_size_ = Length(s.command_) + 4;
          s.actions_ =
              sd::read_actions(s.tx_, std::span{s.rx_}.first(s.receive_size_));
          (void)s.completion_.start(s.device_, s.actions_, kTransferTimeout);
        } else if (state == State::done || state == State::failed) {
          s.running_ = false;
          s.ready_ = state == State::done;
          if (s.ready_) {
            s.error_ = "ok";
          }
          if (s.observer_.completed) {
            s.observer_.completed(s.observer_.context, s.ready_);
          }
        }
      }
    };

    static std::size_t Length(std::uint32_t cmd) {
      return cmd == 17 ? kSectorSize : 16;
    }

    bool Busy() const {
      return running_ || requested_ || reset_requested_ || leased_;
    }

    void SetSpeed(std::uint32_t hz) {
      if (speed_.set) {
        speed_.set(speed_.context, hz);
      }
    }

    std::uint32_t Rate() const {
      return speed_.rate ? speed_.rate(speed_.context) : 0;
    }

    // Verify the received block, then decide the next step.
    Step::Kind Received() {
      const auto payload =
          sd::data(std::span{rx_}.first(receive_size_), Length(command_), 1);
      if (!payload) {
        error_ = "missing/error data token, truncated data or CRC mismatch";
        return Step::Kind::fail;
      }
      if (command_ == 9) {
        const auto parsed = sd::card(*payload);
        if (!parsed) {
          error_ = "unsupported or invalid CSD";
          return Step::Kind::fail;
        }
        card_ = *parsed;
      }
      if (!observer_.next) {
        SetSpeed(std::min(kDataHz, card_.maximum_hz));
        return Step::Kind::finish;
      }
      const auto step = observer_.next(observer_.context, command_, *payload);
      if (step.kind == Step::Kind::read) {
        command_ = step.command;
        argument_ = step.argument;
      } else if (step.kind == Step::Kind::fail) {
        error_ = step.error ? step.error : "diagnostic check failed";
      }
      return step.kind;
    }

    Transport Transfer() {
      return {device_, rx_, Rate(), card_.sectors, pump_};
    }

    void ResetDone(const hal::ResetResult& result) {
      reset_status_ = result.status;
      reset_ready_.store(true, std::memory_order_release);
    }

    hal::spi::Device device_;
    Speed speed_;
    Reset reset_;
    Transport::Pump pump_;
    Observer observer_;
    Initializer initializer_;
    hal::spi::Completion completion_;
    std::atomic<bool> reset_ready_{false};
    hal::Status reset_status_ = hal::Status::ok;
    Machine machine_;
    std::array<std::uint8_t, 6> tx_{};
    std::array<std::uint8_t, kReceiveCapacity> rx_{};
    std::array<hal::spi::Action, 6> actions_{};
    Card card_{};
    std::size_t receive_size_ = 0;
    std::uint32_t command_ = 0, argument_ = 0, ocr_ = 0;
    const char* error_ = "";
    bool requested_ = false, running_ = false, ready_ = false, leased_ = false;
    bool reset_requested_ = false;
  };


}  // namespace daveos::storage::sd
