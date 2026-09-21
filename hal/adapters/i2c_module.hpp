#pragma once

#include <cinttypes>
#include <tuple>

#include "core/schedule/module.hpp"
#include "core/state_machine/state_machine.hpp"
#include "hal/i2c/action.hpp"

namespace daveos::hal {


  // Optional DaveOS command adapter over application-owned named buses.
  // Each bus supplies name(), init(scheduler), deinit(), acquire()/release(),
  // probe_device(address), and statistics(). A task-time lease must exclude
  // other clients through their entire transaction sequences. probe_device()
  // may change the address only under that lease, after previous completion;
  // release() restores the client address. No constructor touches a bus.
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
      return core::Status::ok;
    }

    core::Status Scan() {
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
      requested_ = true;
      return core::Status::ok;
    }

    void Tick() { scanner_.tick(*this); }

    core::Status Stats() {
      std::apply([&](auto*... buses) { (PrintStats(*buses), ...); }, buses_);
      return core::Status::ok;
    }

   private:
    enum class State : std::uint8_t { idle, probing };

    class Scanner : public core::StateMachine<Scanner, State, State::idle, 2> {
     public:
      void Step(State cs, State& ns, I2cModule& self) {
        switch (cs) {
          case State::idle:
            if (self.requested_) {
              self.bus_index_ = 0;
              self.BeginBus();
              ns = State::probing;
            }
            break;
          case State::probing:
            if (const auto result = self.completion_.result()) {
              const bool error = result->status != Status::ok &&
                                 result->status != Status::nack;
              if (error) {
                self.ScanError(result->status);
              } else if (result->status == Status::ok) {
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
      // nano printf does not support 64-bit integers: display low 32 bits.
      I_("%s: reads=%" PRIu32 " writes=%" PRIu32 " completed=%" PRIu32
         " failed=%" PRIu32 " timeouts=%" PRIu32,
         bus.name(), static_cast<std::uint32_t>(s.read_attempts),
         static_cast<std::uint32_t>(s.write_attempts),
         static_cast<std::uint32_t>(s.completed),
         static_cast<std::uint32_t>(s.failed),
         static_cast<std::uint32_t>(s.timed_out));
    }

    void BeginBus() {
      map_.fill(' ');
      address_ = 8;
      StartProbe();
    }

    void StartProbe() {
      Visit(bus_index_, [&](auto& bus) {
        (void)completion_.start(bus.probe_device(address_), actions_, kTimeout);
      });
    }

    void Finish() {
      std::apply([](auto*... buses) { (buses->release(), ...); }, buses_);
      requested_ = false;
      I_("I2C scan complete");
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
    std::array<i2c::Action, 1> actions_{i2c::probe()};
    i2c::Completion completion_;
    std::size_t bus_index_ = 0;
    std::uint8_t address_ = 8;
    bool requested_ = false;
  };


}  // namespace daveos::hal
