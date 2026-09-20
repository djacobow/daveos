#pragma once

#include "boot/journal.h"

namespace daveos::update {


  using Status = core::Status;
  inline constexpr std::size_t kHeaderSize = 128;
  inline constexpr std::size_t kBlockSize = 1024;
  inline constexpr std::size_t kMaxPatches = kBlockSize / 4;

  struct Header {
    std::uint32_t product = 0, revision = 0, image_size = 0;
    std::array<std::uint32_t, 2> addresses{}, crc{};
    // 0: relocatable base A; 1/2: slot-specific A/B image.
    std::uint32_t encoding = 0, package_size = 0;
    util::Version version{};
  };

  Status decode_header(std::span<const std::byte> bytes,
                       const boot::Layout& layout, std::uint32_t destination,
                       Header& header);

  // Stream parser: protocol boundaries need not coincide with package blocks.
  // Patches are sorted 16-bit records (bit15=subtract delta, low8=word index).
  // Retains one <=1 KiB block and at most 512 bytes of local patch records.
  class PackageReader {
   public:
    enum class State { idle, header, patches, data, ready, done, failed };

    void start(const Header& header, std::uint32_t destination);
    std::size_t tick(std::span<const std::byte> input = {});

    void release() { release_requested_ = true; }

    State state() const { return cs; }

    Status status() const { return status_; }

    std::uint32_t offset() const { return offset_; }

    std::span<const std::byte> block() const {
      return std::span(data_).first(length_);
    }

   private:
    std::size_t Copy(std::span<const std::byte> input,
                     std::span<std::byte> output);
    bool Apply();
    Header header_{};
    std::array<std::byte, 12> block_header_{};
    std::array<std::byte, kMaxPatches * 2> patches_{};
    alignas(16) std::array<std::byte, kBlockSize> data_{};
    State cs = State::idle;
    Status status_ = Status::ok;
    std::uint32_t destination_ = 0, offset_ = 0, length_ = 0, patch_count_ = 0;
    std::size_t filled_ = 0;
    bool start_requested_ = false, release_requested_ = false;
  };


}  // namespace daveos::update
