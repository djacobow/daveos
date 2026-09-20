#pragma once

#include "boot/flash.h"
#include "core/state_machine/state_machine.hpp"

namespace daveos::platform::host {


  // Raw NOR-flash image, with addresses translated relative to base. Exclusive
  // ownership, single calling thread. No mapping, virtual dispatch, or heap.
  // Models aligned erase/program and one-to-zero bits, not STM32 ECC cells.
  class FileFlash {
   public:
    struct Geometry {
      std::uint32_t base;
      std::uint32_t size;
      std::uint32_t sector_size;
    };

    enum class OpenMode { existing, create };

    FileFlash(void* clock_context = nullptr,
              core::Time (*clock)(void*) = nullptr)
        : clock_context_(clock_context), clock_(clock) {}

    ~FileFlash();
    FileFlash(const FileFlash&) = delete;
    FileFlash& operator=(const FileFlash&) = delete;

    // create exclusively creates an erased image; never truncates an existing
    // file. Existing files must have exactly geometry.size bytes. Successful
    // mutation completion includes fsync. poll() performs synchronous host I/O;
    // it has no real-time latency guarantee. Clock defaults to steady_clock.
    core::Status open(const char* path, Geometry geometry,
                      OpenMode mode = OpenMode::existing);
    // Drops an operation that has not executed in poll(), like loss of power
    // before a hardware write starts. Does not model a torn in-flight write.
    void close();
    boot::Flash driver();

   private:
    enum class State { idle, execute };
    enum class Operation { erase, program };

    class Machine
        : public core::StateMachine<Machine, State, State::idle,
                                    static_cast<std::size_t>(State::execute) +
                                        1> {
      friend class core::StateMachine<Machine, State, State::idle,
                                      static_cast<std::size_t>(State::execute) +
                                          1>;

      void Step(State cs, State& ns, FileFlash& owner, core::Status& status) {
        owner.Step(cs, ns, status);
      }
    };

    void Step(State cs, State& ns, core::Status& status);
    bool Range(std::uint32_t address, std::size_t size) const;
    core::Status Read(std::uint32_t address, std::span<std::byte> bytes);
    core::Status Erase(std::uint32_t address);
    core::Status Program(std::uint32_t address,
                         std::span<const std::byte> bytes);
    core::Status Poll();
    bool Write(std::uint32_t offset, std::span<const std::byte> bytes);
    core::Time Now() const;

    int fd_ = -1;
    Geometry geometry_{};
    void* clock_context_;
    core::Time (*clock_)(void*);
    Machine machine_;
    Operation operation_ = Operation::erase;
    bool requested_ = false, closed_ = false;
    std::uint32_t address_ = 0;
    std::array<std::byte, 16> word_{};
  };


}  // namespace daveos::platform::host
