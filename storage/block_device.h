#pragma once

#include <cstdint>
#include <span>

namespace daveos::storage {


  inline constexpr std::size_t kSectorBytes = 512;

  // Borrowed, already-initialized media. All calls belong to one
  // cooperative execution thread. acquire/release reserve the media for the
  // mount lifetime (e.g. prevent an SD probe from resetting a mounted card).
  // A read completes synchronously but may pump other tasks through its DI
  // boundary. Never return while hardware still owns the destination buffer.
  struct BlockDevice {
    void* context = nullptr;
    bool (*ready)(void*) = nullptr;
    std::uint64_t (*sectors)(void*) = nullptr;
    bool (*read)(void*, std::uint32_t, std::span<std::uint8_t>) = nullptr;
    bool (*acquire)(void*) = nullptr;
    void (*release)(void*) = nullptr;
    // Optional write capability. Both callbacks are required for writable
    // mounts.
    bool (*write)(void*, std::uint32_t,
                  std::span<const std::uint8_t>) = nullptr;
    bool (*sync)(void*) = nullptr;
  };


}  // namespace daveos::storage
