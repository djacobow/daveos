#pragma once

#include <algorithm>
#include <optional>
#include <vector>

#include "boot/flash.h"

namespace reliability {


  namespace boot = daveos::boot;
  using Status = boot::Status;

  // Stateful fake hardware: mutations happen on poll, including partial writes
  // and erases before a simulated power failure. Never allow rewriting a flash
  // programming unit (models STM32 ECC restrictions even for identical bytes).
  struct MemoryFlash {
    std::vector<std::byte> bytes =
        std::vector<std::byte>(4096, std::byte{0xff});
    std::vector<bool> written = std::vector<bool>(256);
    std::vector<bool> unreadable = std::vector<bool>(256);
    std::uint32_t sector_size = 512;
    std::uint32_t write_size = 16;
    std::uint32_t executing = 2;
    std::vector<std::uint32_t> erases;
    std::uint64_t clock = 0;
    int operations = 0;
    int fail_operation = -1;
    std::size_t partial = 0;

    struct Pending {
      std::uint32_t address;
      std::array<std::byte, 32> data;
      bool erase;
    };

    std::optional<Pending> pending;

    boot::Flash driver() {
      return {
          this,
          [](void* p, std::uint32_t address, std::span<std::byte> output) {
            auto& self = *static_cast<MemoryFlash*>(p);
            if (address > self.bytes.size() ||
                output.size() > self.bytes.size() - address) {
              return Status::invalid_argument;
            }
            for (std::size_t i = address / self.write_size;
                 !output.empty() &&
                 i <= (address + output.size() - 1) / self.write_size;
                 ++i) {
              if (self.unreadable[i]) {
                return Status::io_error;
              }
            }
            std::copy_n(self.bytes.begin() + address, output.size(),
                        output.begin());
            return Status::ok;
          },
          [](void* p, std::uint32_t address) {
            auto& self = *static_cast<MemoryFlash*>(p);
            if (self.pending) {
              return Status::busy;
            }
            if (address % self.sector_size ||
                std::uint64_t(address) + self.sector_size > self.bytes.size()) {
              return Status::invalid_argument;
            }
            self.erases.push_back(address);
            self.pending = Pending{address, {}, true};
            return Status::ok;
          },
          [](void* p, std::uint32_t address, std::span<const std::byte> input) {
            auto& self = *static_cast<MemoryFlash*>(p);
            if (self.pending) {
              return Status::busy;
            }
            if (address % self.write_size || input.size() != self.write_size ||
                std::uint64_t(address) + self.write_size > self.bytes.size() ||
                self.written[address / self.write_size]) {
              return Status::invalid_argument;
            }
            Pending op{address, {}, false};
            std::copy(input.begin(), input.end(), op.data.begin());
            self.pending = op;
            return Status::ok;
          },
          [](void* p) { return static_cast<MemoryFlash*>(p)->Poll(); },
          [](void* p) { return static_cast<MemoryFlash*>(p)->clock += 100; },
          [](void* p) { return static_cast<MemoryFlash*>(p)->executing; }};
    }

    Status Poll() {
      if (!pending) {
        return Status::ok;
      }
      auto operation = *pending;
      pending.reset();
      const bool fail = operations++ == fail_operation;
      auto size = operation.erase ? sector_size : write_size;
      auto count = fail ? std::min<std::size_t>(partial, size) : size;
      for (std::size_t i = 0; i < count; ++i) {
        auto& value = bytes[operation.address + i];
        if (operation.erase) {
          value = std::byte{0xff};
        } else {
          value &= operation.data[i];
        }
      }
      if (operation.erase) {
        for (std::size_t i = 0; i < count / write_size; ++i) {
          written[operation.address / write_size + i] = false;
          unreadable[operation.address / write_size + i] = false;
        }
      } else {
        written[operation.address / write_size] = true;
        unreadable[operation.address / write_size] = fail && count < write_size;
      }
      return fail ? Status::io_error : Status::ok;
    }

    static boot::Layout layout() {
      return {{1024, 3072}, {0, 512}, 1024, 512, 16, 1, 1};
    }
  };


}  // namespace reliability
