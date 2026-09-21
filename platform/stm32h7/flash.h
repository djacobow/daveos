#pragma once

#include "boot/flash.h"

namespace daveos::platform::stm32h7 {


  // M7-owned asynchronous flash for the fixed A/B layout. VTOR in sector zero
  // denotes boot maintenance; standalone apps must not use its mutating API.
  // Sector zero of each bank is always protected;
  // runtime erases and image writes cannot touch the executing application.
  // All instances share ownership until completion has been polled. M4 must
  // remain idle and must not access flash under modification.
  class Flash {
   public:
    Flash(void* clock_context, core::Time (*clock)(void*),
          void (*progress)(void*) = nullptr)
        : context_(clock_context), clock_(clock), progress_(progress) {}

    Flash(const Flash&) = delete;
    Flash& operator=(const Flash&) = delete;
    boot::Flash driver();

   private:
    core::Status Read(std::uint32_t address, std::span<std::byte> bytes);
    core::Status Erase(std::uint32_t address);
    core::Status Program(std::uint32_t address,
                         std::span<const std::byte> bytes);
    core::Status Poll();
    core::Status Unlock(std::uint32_t bank);
    static std::uint32_t ExecutingBank();
    void* context_;
    core::Time (*clock_)(void*);
    void (*progress_)(void*);
    std::uint32_t bank_ = 0, address_ = 0, size_ = 0;
    bool pending_ = false;
  };


}  // namespace daveos::platform::stm32h7
