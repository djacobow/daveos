#pragma once

#include <cstdint>
#include <span>

#include "core/foundation/types.hpp"

namespace daveos::storage {


  inline constexpr std::size_t kSectorBytes = 512;

  // Borrowed, already-initialized media. All calls belong to one
  // cooperative execution thread. acquire/release reserve the media for the
  // mount lifetime (e.g. prevent an SD probe from resetting a mounted card).
  // A read completes synchronously but may pump other tasks through its DI
  // boundary. Never return while hardware still owns the destination buffer.
  //
  // Operations return the cause of a failure rather than a flag:
  //   not_running       media not ready, or not acquired by this caller
  //   busy              already acquired or otherwise in use
  //   invalid_argument  empty, partial-sector or out-of-range request
  //   timeout, io_error, checksum_error  the device failed the transfer
  // Consumers map these once at their own boundary (e.g. the FatFs bridge).
  struct BlockDevice {
    void* context = nullptr;
    bool (*ready)(void*) = nullptr;
    std::uint64_t (*sectors)(void*) = nullptr;
    core::Status (*read)(void*, std::uint32_t,
                         std::span<std::uint8_t>) = nullptr;
    core::Status (*acquire)(void*) = nullptr;
    void (*release)(void*) = nullptr;
    // Optional write capability. Both callbacks are required for writable
    // mounts.
    core::Status (*write)(void*, std::uint32_t,
                          std::span<const std::uint8_t>) = nullptr;
    core::Status (*sync)(void*) = nullptr;
  };


}  // namespace daveos::storage
