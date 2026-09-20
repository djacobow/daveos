#include "package.h"

#include <algorithm>
#include <cstring>

#include "util/crc32.h"
#include "util/wire.h"

namespace daveos::update {


  namespace wire = util::wire;

  Status decode_header(std::span<const std::byte> bytes,
                       const boot::Layout& layout, std::uint32_t destination,
                       Header& header) {
    if (bytes.size() != kHeaderSize || !layout.valid() || destination >= 2) {
      return Status::invalid_argument;
    }
    if (wire::read32(bytes, 0) != 0x50534f44 || wire::read32(bytes, 4) != 1 ||
        wire::read32(bytes, 8) != kHeaderSize ||
        wire::read32(bytes, 44) != kBlockSize ||
        wire::read32(bytes, 108) != 0) {
      return Status::incompatible;
    }
    if (wire::read32(bytes, 124) != util::crc32::calculate(bytes.first(124))) {
      return Status::checksum_error;
    }
    Header result;
    result.product = wire::read32(bytes, 12);
    result.revision = wire::read32(bytes, 16);
    result.image_size = wire::read32(bytes, 20);
    result.addresses = {wire::read32(bytes, 24), wire::read32(bytes, 28)};
    result.crc = {wire::read32(bytes, 32), wire::read32(bytes, 36)};
    result.encoding = wire::read32(bytes, 40);
    result.package_size = wire::read32(bytes, 104);
    auto blocks =
        (std::uint64_t(result.image_size) + kBlockSize - 1) / kBlockSize;
    auto minimum = kHeaderSize + blocks * 12 + result.image_size;
    if (!result.image_size || result.image_size > layout.slot_size ||
        result.product != layout.product ||
        result.revision != layout.revision ||
        result.addresses != layout.slots || result.encoding > 2 ||
        (result.encoding && result.encoding != destination + 1) ||
        result.package_size < minimum ||
        result.package_size > minimum + blocks * kMaxPatches * 2) {
      return Status::incompatible;
    }
    result.version.major = wire::read32(bytes, 48);
    result.version.minor = wire::read32(bytes, 52);
    result.version.build = wire::read32(bytes, 56);
    std::memcpy(result.version.commit, bytes.data() + 60, 41);
    result.version.commit[40] = '\0';
    result.version.dirty = bytes[101] != std::byte{0};
    header = result;
    return Status::ok;
  }

  void PackageReader::start(const Header& header, std::uint32_t destination) {
    header_ = header;
    destination_ = destination;
    start_requested_ = true;
  }

  std::size_t PackageReader::Copy(std::span<const std::byte> input,
                                  std::span<std::byte> output) {
    const auto count = std::min(input.size(), output.size() - filled_);
    std::copy_n(input.begin(), count, output.begin() + filled_);
    filled_ += count;
    return count;
  }

  bool PackageReader::Apply() {
    std::uint32_t previous = 0;
    for (std::uint32_t i = 0; i < patch_count_; ++i) {
      auto code =
          std::uint32_t(std::to_integer<std::uint8_t>(patches_[i * 2])) |
          (std::uint32_t(std::to_integer<std::uint8_t>(patches_[i * 2 + 1]))
           << 8);
      auto at = (code & 0xff) * 4;
      if ((code & 0x7f00) || at + 4 > length_ || (i && at <= previous) ||
          header_.encoding) {
        return false;
      }
      previous = at;
      if (destination_ == 1) {
        auto value = wire::read32(data_, at);
        auto delta = header_.addresses[1] - header_.addresses[0];
        // Explicit uint32 modular arithmetic matches ARM link relocations.
        wire::write32(data_, at, code & 0x8000 ? value - delta : value + delta);
      }
    }
    return true;
  }

  std::size_t PackageReader::tick(std::span<const std::byte> input) {
    State ns = cs;
    std::size_t consumed = 0;
    // Restart is an input accepted in every state (after the owner has stopped
    // using the previous block). All state assignments remain in this switch.
    switch (cs) {
      case State::idle:
      case State::header:
      case State::patches:
      case State::data:
      case State::ready:
      case State::done:
      case State::failed: {
        if (start_requested_) {
          start_requested_ = false;
          release_requested_ = false;
          offset_ = length_ = patch_count_ = 0;
          filled_ = 0;
          status_ = Status::ok;
          ns = State::header;
          break;
        }
        switch (cs) {
          case State::header: {
            consumed = Copy(input, block_header_);
            if (filled_ == block_header_.size()) {
              length_ = wire::read32(block_header_, 4);
              patch_count_ = wire::read32(block_header_, 8);
              if (wire::read32(block_header_, 0) != offset_ ||
                  offset_ >= header_.image_size ||
                  length_ != std::min<std::uint32_t>(
                                 kBlockSize, header_.image_size - offset_) ||
                  patch_count_ > kMaxPatches) {
                status_ = Status::parse_error;
                ns = State::failed;
              } else {
                filled_ = 0;
                ns = patch_count_ ? State::patches : State::data;
              }
            }
            break;
          }
          case State::patches: {
            consumed = Copy(input, std::span(patches_).first(patch_count_ * 2));
            if (filled_ == patch_count_ * 2) {
              filled_ = 0;
              ns = State::data;
            }
            break;
          }
          case State::data: {
            consumed = Copy(input, std::span(data_).first(length_));
            if (filled_ == length_) {
              if (Apply()) {
                ns = State::ready;
              } else {
                status_ = Status::parse_error;
                ns = State::failed;
              }
            }
            break;
          }
          case State::ready: {
            if (release_requested_) {
              release_requested_ = false;
              offset_ += length_;
              filled_ = 0;
              ns = offset_ == header_.image_size ? State::done : State::header;
            }
            break;
          }
          case State::idle:
          case State::done:
          case State::failed: {
            break;
          }
        }
        break;
      }
    }
    if (ns != cs) {
      cs = ns;
    }
    return consumed;
  }


}  // namespace daveos::update
