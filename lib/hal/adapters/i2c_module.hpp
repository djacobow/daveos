#pragma once

#include <atomic>
#include <cinttypes>
#include <limits>
#include <tuple>

#include "core/schedule/module.hpp"
#include "core/state_machine/state_machine.hpp"
#include "hal/i2c/action.hpp"

namespace daveos::hal {


  // Optional DaveOS command adapter over application-owned named buses.
  // Each bus supplies name(), init(scheduler), deinit(), acquire()/release(),
  // probe(address, callback, timeout), reset(callback, timeout), needs_reset(),
  // and statistics(). A task-time lease excludes other clients throughout
  // their transaction sequences. Probes never mutate configured addresses.
  // Startup recovery is deferred until task dispatch. Constructors are passive.
  template <typename Event, typename... Buses>
  class I2cModule : public core::Module<I2cModule<Event, Buses...>, Event> {
    static_assert(sizeof...(Buses) > 0);
    static constexpr auto kPeriod = std::chrono::milliseconds{1};
    static constexpr auto kTimeout = std::chrono::milliseconds{10};
    static constexpr std::size_t kBusCount = sizeof...(Buses);

   public:
    explicit I2cModule(Buses&... buses) : buses_(&buses...) {}

    static constexpr const char* name() { return "i2c"; }

    static constexpr auto tasks() {
      return std::array{DAVEOS_PERIODIC(I2cModule, Tick, kPeriod)};
    }

    static constexpr auto commands() {
      return std::array{
          DAVEOS_COMMAND(I2cModule, Scan, "scan",
                         "Map ACK addresses on all configured I2C buses"),
          DAVEOS_COMMAND(I2cModule, Reset, "reset",
                         "Recover all configured I2C buses"),
          DAVEOS_COMMAND(I2cModule, Stats, "stats",
                         "Show counters for all configured I2C buses")};
    }

    core::Status init(core::InitStage stage) {
      if (stage != core::InitStage::stage1) {
        return core::Status::ok;
      }
      for (std::size_t index = 0; index < kBusCount; ++index) {
        auto status = Status::ok;
        Visit(index, [&](auto& bus) { status = bus.init(this->scheduler()); });
        if (status != Status::ok) {
          for (std::size_t initialized = 0; initialized <= index;
               ++initialized) {
            Visit(initialized, [](auto& bus) { bus.deinit(); });
          }
          return core::Status::initialization_failed;
        }
      }
      bool recover = false;
      std::apply(
          [&](auto*... buses) { ((recover |= buses->needs_reset()), ...); },
          buses_);
      return recover ? Reset() : core::Status::ok;
    }

    core::Status Scan() { return Request(false); }

    core::Status Reset() { return Request(true); }

    void Tick() { scanner_.tick(*this); }

    core::Status Stats() {
      std::apply([&](auto*... buses) { (PrintStats(*buses), ...); }, buses_);
      return core::Status::ok;
    }

   private:
    core::Status Request(bool reset) {
      if (requested_) {
        return core::Status::busy;
      }
      // Task-only, nonblocking reservation; rollback any partial acquisition.
      for (std::size_t index = 0; index < kBusCount; ++index) {
        bool acquired = false;
        Visit(index, [&](auto& bus) { acquired = bus.acquire(); });
        if (!acquired) {
          for (std::size_t previous = 0; previous < index; ++previous) {
            Visit(previous, [](auto& bus) { bus.release(); });
          }
          return core::Status::busy;
        }
      }
      reset_requested_ = reset;
      requested_ = true;
      return core::Status::ok;
    }

    enum class State : std::uint8_t { idle, probing, resetting };

    class Scanner : public core::StateMachine<Scanner, State, State::idle, 3> {
     public:
      void Step(State cs, State& ns, I2cModule& self) {
        switch (cs) {
          case State::idle:
            if (self.requested_) {
              self.bus_index_ = 0;
              self.BeginBus();
              ns = self.reset_requested_ ? State::resetting : State::probing;
            }
            break;
          case State::probing:
            if (self.ready_.load(std::memory_order_acquire)) {
              const auto status = self.completion_status_;
              const bool error = status != Status::ok && status != Status::nack;
              if (error) {
                self.ScanError(status);
              } else if (status == Status::ok) {
                self.map_[self.address_] = '*';
              }
              if (error || ++self.address_ == 0x78) {
                if (!error) {
                  self.PrintMap();
                }
                if (++self.bus_index_ == kBusCount) {
                  self.Finish();
                  ns = State::idle;
                } else {
                  self.BeginBus();
                }
              } else {
                self.StartProbe();
              }
            }
            break;
          case State::resetting:
            if (self.ready_.load(std::memory_order_acquire)) {
              self.Visit(self.bus_index_,
                         [&](auto& bus) { self.ReportReset(bus.name()); });
              if (++self.bus_index_ == kBusCount) {
                self.Finish();
                ns = State::idle;
              } else {
                self.BeginBus();
              }
            }
            break;
        }
      }
    };

    template <typename Function>
    void Visit(std::size_t target, Function function) {
      std::size_t index = 0;
      std::apply(
          [&](auto*... buses) {
            ((index++ == target ? (void)function(*buses) : (void)0), ...);
          },
          buses_);
    }

    template <typename Bus>
    void PrintStats(Bus& bus) {
      [[maybe_unused]] const auto s = bus.statistics();
      // Saturate displayed values and mark overflow; never wrap silently.
      I_("%s: reads=%" PRIu32 "%s writes=%" PRIu32 "%s completed=%" PRIu32
         "%s failed=%" PRIu32 "%s timeouts=%" PRIu32 "%s",
         bus.name(), Display(s.read_attempts), Overflow(s.read_attempts),
         Display(s.write_attempts), Overflow(s.write_attempts),
         Display(s.completed), Overflow(s.completed), Display(s.failed),
         Overflow(s.failed), Display(s.timed_out), Overflow(s.timed_out));
    }

    static std::uint32_t Display(std::uint64_t value) {
      return static_cast<std::uint32_t>(
          std::min(value, std::uint64_t{UINT32_MAX}));
    }

    static const char* Overflow(std::uint64_t value) {
      return value > UINT32_MAX ? "+" : "";
    }

    void Publish(Status status) {
      completion_status_ = status;
      ready_.store(true, std::memory_order_release);
    }

    void ProbeDone(const i2c::Result& result) { Publish(result.status); }

    void ResetDone(const ResetResult& result) { Publish(result.status); }

    void BeginBus() {
      map_.fill(' ');
      address_ = 8;
      if (!reset_requested_) {
        StartProbe();
        return;
      }
      ready_.store(false, std::memory_order_relaxed);
      Visit(bus_index_, [&](auto& bus) {
        const auto status = bus.reset(
            Callback<ResetResult>::template bind<&I2cModule::ResetDone>(*this),
            std::chrono::milliseconds{100});
        if (status != Status::ok) {
          Publish(status);
        }
      });
    }

    void StartProbe() {
      ready_.store(false, std::memory_order_relaxed);
      Visit(bus_index_, [&](auto& bus) {
        const auto status = bus.probe(
            i2c::Address{address_},
            i2c::Callback::template bind<&I2cModule::ProbeDone>(*this),
            kTimeout);
        if (status != Status::ok) {
          Publish(status);
        }
      });
    }

    void ReportReset([[maybe_unused]] const char* name) {
      if (completion_status_ == Status::ok) {
        I_("%s reset: ok", name);
      } else {
        E_("%s reset: %s", name, enum_name(completion_status_));
      }
    }

    void Finish() {
      std::apply([](auto*... buses) { (buses->release(), ...); }, buses_);
      requested_ = false;
      I_("I2C %s complete", reset_requested_ ? "reset" : "scan");
    }

    void ScanError([[maybe_unused]] Status status) {
      Visit(bus_index_, [&](auto& bus) {
        E_("%s scan aborted at 0x%02" PRIx32 ": %s", bus.name(),
           static_cast<std::uint32_t>(address_), enum_name(status));
      });
    }

    void PrintMap() {
      Visit(bus_index_, [&](auto& bus) {
        I_("%s ACK map (7-bit; reserved addresses not probed)", bus.name());
        I_("   0 1 2 3 4 5 6 7 8 9 a b c d e f");
        for (std::uint32_t row = 0; row < 8; ++row) {
          std::array<char, 31> cells;
          cells.fill(' ');
          for (std::size_t column = 0; column < 16; ++column) {
            cells[column * 2] = map_[row * 16 + column];
          }
          I_("%02" PRIx32 " %.*s", row * 16, 31, cells.data());
        }
      });
    }

    std::tuple<Buses*...> buses_;
    Scanner scanner_;
    std::array<char, 128> map_{};
    std::atomic<bool> ready_{false};
    Status completion_status_ = Status::ok;
    std::size_t bus_index_ = 0;
    std::uint8_t address_ = 8;
    bool requested_ = false, reset_requested_ = false;
  };


}  // namespace daveos::hal
