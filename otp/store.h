#pragma once

#include "driver.h"

namespace daveos::otp {


#define DAVEOS_OTP_PHASES(X) X(none) X(scan) X(program) X(verify) X(lock)
  DAVEOS_ENUM(Phase, std::uint8_t, DAVEOS_OTP_PHASES)
#undef DAVEOS_OTP_PHASES

  struct Snapshot {
    const char* backend = nullptr;
    bool ready = false;
    std::uint32_t consumed = 0, remaining = kBlockCount, gaps = 0;
    std::uint32_t invalid = 0, unknown = 0, unreadable = 0;
    std::optional<std::uint32_t> serial_block;
    bool serial_locked = false;
    std::uint32_t errors = 0;
    Phase phase = Phase::none;
    std::optional<std::uint32_t> error_block;
    Status error = Status::ok;
  };

  // Fixed-size RAM cache; independent of scheduler, commands and logging.
  // Borrowed driver/CRC dependencies must outlive the Store. Constructors do
  // nothing to them. Single thread only, including initialization callbacks.
  class Store {
   public:
    explicit Store(Driver driver,
                   const util::crc32::Service& crc = util::crc32::Service{})
        : driver_(driver), crc_(crc) {}

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    Status init();

    bool ready() const { return ready_; }

    // Views live until the next verified value change or Store destruction.
    // A lock-only retry or identical write leaves the view intact.
    std::optional<std::string_view> serial() const;
    Status set_serial(std::string_view value);
    Snapshot snapshot() const;

   private:
    enum class Kind { unused, valid, invalid, unknown, unreadable };
    Kind Classify(const Block& block) const;
    Status Error(Status status, Phase phase,
                 std::optional<std::uint32_t> block = {});
    Status Lock(std::uint32_t block);
    Driver driver_;
    util::crc32::Service crc_;
    std::array<Kind, kBlockCount> kinds_{};
    Record serial_{};
    std::optional<std::uint32_t> serial_block_;
    std::uint32_t next_ = 0;
    bool initialized_ = false, ready_ = false, locked_ = false;
    Snapshot diagnostics_{};
  };


}  // namespace daveos::otp
