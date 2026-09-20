#pragma once

#include "boot/flash.h"
#include "driver.h"

namespace daveos::otp {


  // Write-once record emulation in one externally reserved 8 KiB sector.
  // Never erases. The caller owns initialization and controller serialization
  // of Flash; separate hardware adapters must share peripheral ownership.
  class FlashOtp {
   public:
    static constexpr std::uint32_t kSlotSize = 256;
    static constexpr std::uint32_t kSize = kBlockCount * kSlotSize;
    static constexpr core::Time kWriteTimeout = 100000;
    static constexpr std::uint32_t kPollBudget = 1000000;

    FlashOtp(boot::Flash flash, std::uint32_t base)
        : flash_(flash), base_(base) {}

    FlashOtp(const FlashOtp&) = delete;
    FlashOtp& operator=(const FlashOtp&) = delete;
    Driver driver();

   private:
    using Word = std::array<std::byte, 16>;
    using Image = std::array<std::byte, kSlotSize>;
    Status Init();
    Status Load(std::uint32_t block, Image& image, std::array<bool, 16>& bad);
    Status Inspect(std::uint32_t block, Block& output);
    ProgramResult Program(std::uint32_t block, const Record& record);
    Status Lock(std::uint32_t block);
    ProgramResult Write(std::uint32_t block, std::uint32_t offset,
                        const Word& word);
    boot::Flash flash_;
    std::uint32_t base_;
    // Retain program data even if a timed-out operation is still in flight.
    Word pending_{};
    std::array<bool, kBlockCount> attempted_{};
    std::array<std::uint8_t, kBlockCount> lock_next_{};
    bool initialized_ = false, timed_out_ = false;
  };


}  // namespace daveos::otp
