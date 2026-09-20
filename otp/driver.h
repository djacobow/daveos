#pragma once

#include "core/platform/platform.hpp"
#include "record.h"

namespace daveos::otp {


  using Status = core::Status;
  enum class Storage { unused, consumed, unreadable };

  struct Block {
    Record record;
    Storage storage = Storage::unused;
    bool locked = false;
  };

  struct ProgramResult {
    Status status;
    // True once programming may have touched storage, even if it failed.
    bool attempted;
  };

  // Borrowed synchronous backend. inspect reports recoverable block ECC via
  // Storage::unreadable; a non-ok status means inspection itself failed.
  // program must reject used/locked blocks and commit the type field last.
  // No erase API, allocation, implicit retries, or ISR/concurrent access.
  struct Driver {
    void* context = nullptr;
    const char* name = nullptr;
    std::uint32_t blocks = kBlockCount;
    std::size_t block_size = kRecordSize;
    Status (*init)(void*) = nullptr;
    Status (*inspect)(void*, std::uint32_t, Block&) = nullptr;
    ProgramResult (*program)(void*, std::uint32_t, const Record&) = nullptr;
    Status (*lock)(void*, std::uint32_t) = nullptr;

    bool valid() const { return name && init && inspect && program && lock; }
  };


}  // namespace daveos::otp
