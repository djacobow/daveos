#pragma once

#include "otp/driver.h"

namespace daveos::platform::host {


  // Persistent, write-once OTP model. init opens the borrowed path;
  // construction does no I/O. Exactly one FileOtp may own the file. All buffers
  // are bounded.
  class FileOtp {
   public:
    enum class OpenMode { existing, create };

    explicit FileOtp(const char* path, OpenMode mode = OpenMode::existing)
        : path_(path), mode_(mode) {}

    ~FileOtp();
    FileOtp(const FileOtp&) = delete;
    FileOtp& operator=(const FileOtp&) = delete;
    otp::Driver driver();
    void close();

    // One-shot failure after N persisted programming/lock steps (zero means
    // before the first mutation). A halfword has intent, data and completion
    // steps. This models interruption without erasing prior writes on reopen.
    void fail_after(std::optional<std::uint32_t> steps) { failure_ = steps; }

    // Persist a modeled per-halfword ECC read failure; fixture tooling only.
    otp::Status inject_read_error(std::uint32_t block, std::uint32_t halfword);

   private:
    static constexpr std::size_t kHeaderSize = 64, kImageSize = 128;
    using Image = std::array<std::byte, kImageSize>;
    enum class Word : std::uint8_t {
      virgin,
      attempted,
      programmed,
      unreadable
    };
    otp::Status Init();
    otp::Status Inspect(std::uint32_t index, otp::Block& block);
    otp::ProgramResult Program(std::uint32_t index, const otp::Record& record);
    otp::Status Lock(std::uint32_t index);
    bool Read(std::size_t offset, std::span<std::byte> data) const;
    bool Write(std::size_t offset, std::span<const std::byte> data);
    bool Persist(std::size_t offset, std::span<const std::byte> data);
    otp::Status Load(std::uint32_t index, Image& image) const;

    static std::size_t Offset(std::uint32_t index) {
      return kHeaderSize + index * kImageSize;
    }

    const char* path_;
    OpenMode mode_;
    int fd_ = -1;
    std::optional<std::uint32_t> failure_;
    bool attempted_ = false;
  };


}  // namespace daveos::platform::host
