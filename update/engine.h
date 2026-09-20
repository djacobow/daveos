#pragma once

#include "core/state_machine/state_machine.hpp"
#include "package.h"
#include "util/crc32.h"
#include "writer.h"

namespace daveos::update {


  // Transport-independent OTA owner. One task/thread calls all methods; IRQs
  // buffer data elsewhere. Public mutators submit requests; tick owns state.
  class Engine {
   public:
    enum class State {
      disabled,
      idle,
      invalidating,
      receiving,
      writing,
      verifying,
      committing,
      done,
      aborting,
      failed
    };

    Engine(boot::Flash flash, boot::Layout layout, std::uint32_t running_slot,
           util::crc32::Service crc = util::crc32::Service{},
           core::Time inactivity = 30000000)
        : flash_(flash),
          layout_(layout),
          running_slot_(running_slot),
          journal_(flash, layout),
          writer_(flash, layout),
          crc_(crc),
          inactivity_(inactivity) {}

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;

    void enable(bool value) { enabled_ = value; }

    // Platform identity is bound during initialization, never during
    // construction.
    Status running_slot(std::uint32_t slot) {
      if (enabled_ || state() != State::disabled) {
        return Status::busy;
      }
      if (slot >= 2) {
        return Status::invalid_argument;
      }
      running_slot_ = slot;
      return Status::ok;
    }

    bool enabled() const { return enabled_; }

    Status begin(std::span<const std::byte> header);
    Status chunk(std::uint32_t offset, std::span<const std::byte> data,
                 std::uint32_t crc);
    void abort(Status reason = Status::rejected);
    void tick();

    State state() const { return machine_.state(); }

    std::uint64_t dwell_count() const { return machine_.dwell_count(); }

    core::StateStatistics statistics(State state) const {
      return machine_.statistics(state);
    }

    auto statistics() const { return machine_.statistics(); }

    Status status() const { return status_; }

    std::uint32_t next_offset() const { return received_; }

    std::uint32_t total() const {
      return header_.package_size > kHeaderSize
                 ? header_.package_size - kHeaderSize
                 : 0;
    }

    bool ready() const;
    bool active() const;

   private:
    class Machine
        : public core::StateMachine<Machine, State, State::disabled,
                                    static_cast<std::size_t>(State::failed) +
                                        1> {
      friend class core::StateMachine<Machine, State, State::disabled,
                                      static_cast<std::size_t>(State::failed) +
                                          1>;

      void Step(State cs, State& ns, Engine& owner) { owner.Step(cs, ns); }
    };

    void Step(State cs, State& ns);
    void Fail(Status status);
    boot::Flash flash_;
    boot::Layout layout_;
    std::uint32_t running_slot_;
    boot::Journal journal_;
    Writer writer_;
    PackageReader reader_;
    util::crc32::Service crc_;
    Header header_{};
    boot::Snapshot snapshot_{};
    std::array<std::byte, kBlockSize> rx_{};
    std::array<std::byte, 256> verification_{};
    Machine machine_;
    Status status_ = Status::not_running, abort_reason_ = Status::rejected;
    std::uint32_t received_ = 0, verify_offset_ = 0, verify_crc_ = 0;
    std::size_t rx_size_ = 0, rx_at_ = 0;
    core::Time waiting_since_ = 0, inactivity_;
    bool enabled_ = false, requested_ = false, abort_requested_ = false;
  };


}  // namespace daveos::update
