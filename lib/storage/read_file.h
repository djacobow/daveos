#pragma once

#include <span>
#include <string_view>

#include "core/foundation/types.hpp"

namespace daveos::storage {

  // Borrowed, synchronous file operations, called only from a task. read/seek/
  // close may cooperatively yield, but must release all I/O buffer ownership
  // before returning. reserve/release do no I/O; reserve requires a mounted,
  // read-only volume and excludes all competing filesystem operations.
  struct ReadFile {
    void* context = nullptr;
    core::Status (*reserve)(void*) = nullptr;
    void (*release)(void*) = nullptr;
    core::Status (*open)(void*, std::string_view) = nullptr;
    core::Status (*read)(void*, std::span<std::byte>, std::uint32_t&) = nullptr;
    core::Status (*seek)(void*, std::uint32_t) = nullptr;
    core::Status (*close)(void*) = nullptr;

    explicit operator bool() const {
      return reserve && release && open && read && seek && close;
    }
  };


}  // namespace daveos::storage
