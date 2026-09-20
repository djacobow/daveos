#pragma once

#include "boot/flash.h"
#include "core/state_machine/state_machine.hpp"

namespace daveos::update {


  using Status = core::Status;

  // One image block at a time; buffer borrowed until status is terminal.
  // Erases on demand, one sector at a time; program granularity is separate
  // from both package and transport granularity. v1 supports 16-byte writes.
  class Writer {
   public:
    Writer(boot::Flash flash, const boot::Layout& layout)
        : flash_(flash), layout_(layout) {}

    Status begin(std::uint32_t slot, std::uint32_t offset,
                 std::span<const std::byte> data);
    void tick();

    void cancel() { cancelled_ = true; }

    Status status() const { return status_; }

   private:
    enum class State { idle, prepare, erase_wait, program_wait, done, failed };

    class Machine
        : public core::StateMachine<Machine, State, State::idle,
                                    static_cast<std::size_t>(State::failed) +
                                        1> {
      friend class core::StateMachine<Machine, State, State::idle,
                                      static_cast<std::size_t>(State::failed) +
                                          1>;

      void Step(State cs, State& ns, Writer& owner) { owner.Step(cs, ns); }
    };

    void Step(State cs, State& ns);
    boot::Flash flash_;
    boot::Layout layout_;
    Machine machine_;
    Status status_ = Status::ok;
    std::span<const std::byte> data_{};
    alignas(16) std::array<std::byte, 16> word_{};
    std::uint32_t base_ = 0, offset_ = 0, erased_until_ = 0;
    std::size_t written_ = 0;
    core::Time operation_started_ = 0;
    bool requested_ = false, cancelled_ = false;
  };


}  // namespace daveos::update
