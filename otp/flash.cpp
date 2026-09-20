#include "flash.h"

#include <algorithm>
#include <limits>

#include "util/wire.h"

namespace daveos::otp {


  namespace {
    constexpr std::uint32_t kClaim = 0x3143504f, kCommit = 0x3144504f,
                            kLock = 0x314c504f;
    constexpr std::uint32_t kBodyOffset = 16, kCommitOffset = 80,
                            kLockOffset = 96;
    namespace wire = util::wire;
    using Word = std::array<std::byte, 16>;

    bool Erased(std::span<const std::byte> bytes) {
      return std::all_of(bytes.begin(), bytes.end(),
                         [](auto b) { return b == std::byte{0xff}; });
    }

    Word Marker(std::uint32_t kind, std::uint32_t block, std::uint32_t tag,
                std::span<const std::byte> body = {}) {
      Word word{};
      wire::write32(word, 0, kind);
      wire::write32(word, 4, block);
      wire::write32(word, 8, tag);
      auto crc = util::crc32::calculate(std::span(word).first(12));
      wire::write32(word, 12, util::crc32::update(crc, body));
      return word;
    }

    bool Matches(std::span<const std::byte> bytes, const Word& marker) {
      return std::equal(bytes.begin(), bytes.end(), marker.begin(),
                        marker.end());
    }
  }  // namespace

  Status FlashOtp::Init() {
    if (initialized_) {
      return Status::already_initialized;
    }
    if (!flash_.valid() || base_ % kSize ||
        base_ > std::numeric_limits<std::uint32_t>::max() - kSize) {
      return Status::incompatible;
    }
    initialized_ = true;
    return Status::ok;
  }

  Status FlashOtp::Load(std::uint32_t block, Image& image,
                        std::array<bool, 16>& bad) {
    if (!initialized_) {
      return Status::not_running;
    }
    if (timed_out_) {
      return Status::timeout;
    }
    if (block >= kBlockCount) {
      return Status::invalid_argument;
    }
    image.fill(std::byte{0xff});
    for (std::uint32_t i = 0; i < bad.size(); ++i) {
      auto status =
          flash_.read(flash_.context, base_ + block * kSlotSize + i * 16,
                      std::span(image).subspan(i * 16, 16));
      // The flash contract reports guarded per-address ECC as io_error.
      // Other failures (busy, unavailable, range errors) stop the operation.
      bad[i] = status == Status::io_error;
      if (status != Status::ok && !bad[i]) {
        return status;
      }
    }
    return Status::ok;
  }

  Status FlashOtp::Inspect(std::uint32_t block, Block& output) {
    Image image;
    std::array<bool, 16> bad{};
    auto status = Load(block, image, bad);
    if (status != Status::ok) {
      return status;
    }
    output = {};
    output.record.bytes.fill(std::byte{0xff});
    const auto bytes = std::span(image);
    const auto body = bytes.subspan(kBodyOffset, kRecordSize);
    if (!attempted_[block] && Erased(image) &&
        std::none_of(bad.begin(), bad.end(), [](bool b) { return b; })) {
      return Status::ok;
    }
    output.storage = Storage::consumed;
    if (std::any_of(bad.begin(), bad.begin() + 6, [](bool b) { return b; })) {
      output.storage = Storage::unreadable;
      return Status::ok;
    }
    auto type = wire::read32(bytes, kCommitOffset + 8);
    if (!Matches(bytes.first(16), Marker(kClaim, block, 1)) || type >= 0xffff ||
        !Matches(bytes.subspan(kCommitOffset, 16),
                 Marker(kCommit, block, type, body))) {
      return Status::ok;
    }
    std::copy(body.begin(), body.end(), output.record.bytes.begin());
    output.record.bytes[0] = std::byte(type & 0xff);
    output.record.bytes[1] = std::byte(type >> 8);
    auto lock = Marker(kLock, block, wire::read32(output.record.bytes, 4));
    for (std::size_t i = kLockOffset / 16; i < bad.size(); ++i) {
      output.locked |= !bad[i] && Matches(bytes.subspan(i * 16, 16), lock);
    }
    return Status::ok;
  }

  ProgramResult FlashOtp::Write(std::uint32_t block, std::uint32_t offset,
                                const Word& word) {
    pending_ = word;
    auto status = flash_.program(flash_.context,
                                 base_ + block * kSlotSize + offset, pending_);
    if (status != Status::ok) {
      return {status, false};
    }
    attempted_[block] = true;
    const auto start = flash_.now(flash_.context);
    for (std::uint32_t i = 0; i < kPollBudget; ++i) {
      status = flash_.poll(flash_.context);
      if (status != Status::busy) {
        return {status, true};
      }
      if (flash_.now(flash_.context) - start >= kWriteTimeout) {
        break;
      }
    }
    // Preserve the buffer and deny further operations until reconstruction.
    // Never abandon ownership and then poll somebody else's pending write.
    timed_out_ = true;
    return {Status::timeout, true};
  }

  ProgramResult FlashOtp::Program(std::uint32_t block, const Record& record) {
    Block before;
    auto status = Inspect(block, before);
    if (status != Status::ok) {
      return {status, false};
    }
    if (before.storage != Storage::unused || before.locked) {
      return {Status::rejected, false};
    }
    auto result = Write(block, 0, Marker(kClaim, block, 1));
    if (result.status != Status::ok) {
      return result;
    }
    auto body = record.bytes;
    body[0] = body[1] = std::byte{0xff};
    for (std::uint32_t offset = 0; offset < kRecordSize; offset += 16) {
      // An all-ones flash word carries no data. Leave it virgin: this also
      // keeps virgin classification conservative after interrupted padding.
      auto bytes = std::span(body).subspan(offset, 16);
      if (Erased(bytes)) {
        continue;
      }
      Word word;
      std::copy(bytes.begin(), bytes.end(), word.begin());
      result = Write(block, kBodyOffset + offset, word);
      if (result.status != Status::ok) {
        return {result.status, true};
      }
    }
    result = Write(block, kCommitOffset,
                   Marker(kCommit, block, record.type(), body));
    return {result.status, true};
  }

  Status FlashOtp::Lock(std::uint32_t block) {
    Block current;
    auto status = Inspect(block, current);
    if (status != Status::ok || current.locked) {
      return status;
    }
    if (current.storage != Storage::consumed || !current.record.valid()) {
      return Status::rejected;
    }
    Image image;
    std::array<bool, 16> bad{};
    status = Load(block, image, bad);
    if (status != Status::ok) {
      return status;
    }
    std::uint32_t next = kLockOffset / 16;
    for (std::uint32_t i = next; i < bad.size(); ++i) {
      if (bad[i] || !Erased(std::span(image).subspan(i * 16, 16))) {
        next = i + 1;
      }
    }
    next = std::max(next, std::uint32_t(lock_next_[block]));
    if (next == bad.size()) {
      return Status::io_error;
    }
    auto result =
        Write(block, next * 16,
              Marker(kLock, block, wire::read32(current.record.bytes, 4)));
    if (result.attempted) {
      lock_next_[block] = std::uint8_t(next + 1);
    }
    return result.status;
  }

  Driver FlashOtp::driver() {
    return {this,
            "flash-emulator",
            kBlockCount,
            kRecordSize,
            [](void* p) { return static_cast<FlashOtp*>(p)->Init(); },
            [](void* p, std::uint32_t i, Block& b) {
              return static_cast<FlashOtp*>(p)->Inspect(i, b);
            },
            [](void* p, std::uint32_t i, const Record& r) {
              return static_cast<FlashOtp*>(p)->Program(i, r);
            },
            [](void* p, std::uint32_t i) {
              return static_cast<FlashOtp*>(p)->Lock(i);
            }};
  }


}  // namespace daveos::otp
