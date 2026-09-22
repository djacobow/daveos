#include "fault.h"

#include <atomic>

namespace daveos::util::fault {


  namespace {
    constexpr std::uint32_t kMagic = 0x46534f44;
  }

  void save(Record& record, const Data& data) {
    Record next{};
    auto& bytes = next.bytes;
    wire::write32(bytes, 0, kMagic);
    wire::write32(bytes, 4, 1);
    wire::write32(bytes, 8, bytes.size());
    wire::write32(bytes, 12, static_cast<std::uint32_t>(data.kind));
    wire::write32(bytes, 16, data.version.major);
    wire::write32(bytes, 20, data.version.minor);
    wire::write32(bytes, 24, data.version.build);
    std::memcpy(bytes.data() + 28, data.version.commit, 41);
    bytes[69] = std::byte(data.version.dirty);
    bytes[70] = std::byte(data.frame_valid);
    wire::write64(bytes, 72, data.installation);
    wire::write32(bytes, 80, data.status);
    for (std::size_t i = 0; i < data.frame.size(); ++i) {
      wire::write32(bytes, 84 + i * 4, data.frame[i]);
    }
    wire::write32(bytes, 116, data.cfsr);
    wire::write32(bytes, 120, data.hfsr);
    wire::write32(bytes, 124, data.mmfar);
    wire::write32(bytes, 128, data.bfar);
    wire::write32(bytes, 132, data.exc_return);
    wire::write32(bytes, 136, data.stack);
    std::memcpy(bytes.data() + 140, data.check, 32);
    std::memcpy(bytes.data() + 172, data.module, 32);
    std::memcpy(bytes.data() + 204, data.task, 32);
    wire::write64(bytes, 236, data.expected);
    wire::write64(bytes, 244, data.completed);
    wire::write32(bytes, 252, crc32::calculate(std::span(bytes).first(252)));
    // Invalidate before replacement and publish magic last. Platform handler
    // issues the required cache clean/barriers before reset on cached targets.
    clear(record);
    std::memcpy(record.bytes.data() + 4, bytes.data() + 4, bytes.size() - 4);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    wire::write32(record.bytes, 0, kMagic);
  }

  bool read(const Record& record, Data& data) {
    const auto& bytes = record.bytes;
    if (wire::read32(bytes, 0) != kMagic || wire::read32(bytes, 4) != 1 ||
        wire::read32(bytes, 8) != bytes.size() ||
        wire::read32(bytes, 12) > static_cast<std::uint32_t>(Kind::boot) ||
        wire::read32(bytes, 252) !=
            crc32::calculate(std::span(bytes).first(252))) {
      return false;
    }
    Data result;
    result.kind = static_cast<Kind>(wire::read32(bytes, 12));
    result.version.major = wire::read32(bytes, 16);
    result.version.minor = wire::read32(bytes, 20);
    result.version.build = wire::read32(bytes, 24);
    std::memcpy(result.version.commit, bytes.data() + 28, 41);
    result.version.commit[40] = '\0';
    result.version.dirty = bytes[69] != std::byte{0};
    result.frame_valid = bytes[70] != std::byte{0};
    result.installation = wire::read64(bytes, 72);
    result.status = wire::read32(bytes, 80);
    for (std::size_t i = 0; i < result.frame.size(); ++i) {
      result.frame[i] = wire::read32(bytes, 84 + i * 4);
    }
    result.cfsr = wire::read32(bytes, 116);
    result.hfsr = wire::read32(bytes, 120);
    result.mmfar = wire::read32(bytes, 124);
    result.bfar = wire::read32(bytes, 128);
    result.exc_return = wire::read32(bytes, 132);
    result.stack = wire::read32(bytes, 136);
    std::memcpy(result.check, bytes.data() + 140, 32);
    std::memcpy(result.module, bytes.data() + 172, 32);
    std::memcpy(result.task, bytes.data() + 204, 32);
    result.check[31] = result.module[31] = result.task[31] = '\0';
    result.expected = wire::read64(bytes, 236);
    result.completed = wire::read64(bytes, 244);
    data = result;
    return true;
  }

  void clear(Record& record) {
    wire::write32(record.bytes, 0, 0);
    std::atomic_signal_fence(std::memory_order_seq_cst);
  }


}  // namespace daveos::util::fault
