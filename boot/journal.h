#pragma once

#include "core/state_machine/state_machine.hpp"
#include "flash.h"
#include "util/version.h"

namespace daveos::boot {


  // Persistent image eligibility, changed only through committed snapshots.
  enum class ImageState : std::uint32_t {
    empty,
    incomplete,
    pending,
    trial,
    confirmed,
    rejected
  };

  struct Image {
    std::uint64_t installation = 0;
    std::uint32_t size = 0;
    std::uint32_t crc = 0;
    ImageState state = ImageState::empty;
    std::uint32_t product = 0;
    std::uint32_t revision = 0;
    util::Version version{};
  };

  struct Snapshot {
    std::uint64_t sequence = 0;
    std::uint64_t counter = 0;
    std::array<Image, 2> images{};
  };

  inline constexpr std::size_t kRecordSize = 256;
  using Record = std::array<std::byte, kRecordSize>;

  // Little-endian journal: v1 uses 16-byte writes; v2 isolates its commit
  // trailer in the final 32-byte unit. Factory tooling uses the same formats.
  Record encode(const Snapshot& snapshot, std::uint32_t write_size = 16);
  bool decode(const Record& record, Snapshot& snapshot);

  // Two independently erasable regions; the last valid committed record is
  // never erased until an equivalent or newer snapshot has been committed and
  // verified in the other region. No heap.
  class Journal {
   public:
    Journal(Flash flash, const Layout& layout)
        : flash_(flash), layout_(layout) {}

    Status load(Snapshot& snapshot);
    Status begin(const Snapshot& snapshot, bool initialize = false);
    void tick();
    // Boot-only maintenance: preserve the newest record in the other bank,
    // then erase this bank's checkpoint area before handing off to its app.
    Status prepare_runtime(std::uint32_t bank, core::Time timeout = 5000000);

    Status status() const { return status_; }

    // Bounded foreground convenience for confirmation/boot selection. OTA can
    // instead call begin/tick cooperatively. timeout includes erase/program.
    Status commit(const Snapshot& snapshot, core::Time timeout = 0,
                  bool initialize = false);

   private:
    enum class State {
      idle,
      erase,
      erase_wait,
      write,
      write_wait,
      verify,
      done,
      failed
    };

    class Machine
        : public core::StateMachine<Machine, State, State::idle,
                                    static_cast<std::size_t>(State::failed) +
                                        1> {
      friend class core::StateMachine<Machine, State, State::idle,
                                      static_cast<std::size_t>(State::failed) +
                                          1>;

      void Step(State cs, State& ns, Journal& owner) { owner.Step(cs, ns); }
    };

    void Step(State cs, State& ns);
    Flash flash_;
    Layout layout_;
    Record record_{};
    Record pending_record_{};
    std::uint32_t pending_target_ = 0;
    bool checkpoint_ = false;
    bool erase_only_ = false;
    Machine machine_;
    Status status_ = Status::ok;
    std::uint32_t target_ = 0;
    std::uint32_t latest_ = 0;
    std::size_t offset_ = 0;
    core::Time operation_started_ = 0;
    bool requested_ = false;
    bool erase_needed_ = false;
    bool timed_out_ = false;
  };


}  // namespace daveos::boot
