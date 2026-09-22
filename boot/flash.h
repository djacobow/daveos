#pragma once

#include <array>
#include <span>

#include "core/foundation/types.hpp"

namespace daveos::boot {


  using Status = core::Status;

  // Geometry is immutable for a layout revision. Both application slots start
  // at the same offset in their physical bank; metadata resides before them.
  struct Layout {
    std::array<std::uint32_t, 2> slots{};
    std::array<std::uint32_t, 2> metadata{};
    std::uint32_t slot_size = 0;
    std::uint32_t sector_size = 8192;
    std::uint32_t write_size = 16;
    std::uint32_t product = 0;
    std::uint32_t revision = 1;
    core::Time operation_timeout = 1000000;

    bool valid() const;
  };

  // Borrowed, single-operation flash device.
  //
  // Admission: erase() and program() start one operation and return; a
  //   rejected start leaves nothing in progress. read() is synchronous.
  // Ownership: program() data stays borrowed and unchanged until poll()
  //   reports a terminal status.
  // Execution: all calls from one task/thread; now() is monotonic
  //   microseconds and works before scheduler dispatch.
  // Deadline: callers bound each operation with Layout::operation_timeout,
  //   polling until then.
  // Failure: poll() returns busy until completion, then ok or the terminal
  //   error. read() reports ECC and read errors. No retries.
  // Lifetime: the device outlives its users and any operation in progress.
  struct Flash {
    void* context = nullptr;
    Status (*read)(void*, std::uint32_t, std::span<std::byte>) = nullptr;
    Status (*erase)(void*, std::uint32_t) = nullptr;
    Status (*program)(void*, std::uint32_t,
                      std::span<const std::byte>) = nullptr;
    Status (*poll)(void*) = nullptr;
    core::Time (*now)(void*) = nullptr;

    // Optional runtime bank constraint: 0/1 identifies the executing bank;
    // 2 permits boot-time maintenance. Callbacks remain borrowed.
    std::uint32_t (*executing_bank)(void*) = nullptr;

    bool valid() const { return read && erase && program && poll && now; }
  };


}  // namespace daveos::boot
