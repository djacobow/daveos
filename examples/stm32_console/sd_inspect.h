#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "storage/sd/protocol.h"

// Read-only demo decoders. No packed-struct casts, allocations or filesystem
// mounting. Unknown layouts remain unknown; labels never determine FAT type.
namespace app::sd {


  using daveos::storage::sd::card;
  using daveos::storage::sd::Card;
  using daveos::storage::sd::crc16;
  using daveos::storage::sd::data;
  using daveos::storage::sd::little;

  struct Fat {
    const char* name;
    std::uint32_t sectors;
    std::uint32_t cluster_bytes;
    std::uint32_t clusters;
  };

  inline bool signature(std::span<const std::uint8_t> b) {
    return b.size() == 512 && b[510] == 0x55 && b[511] == 0xaa;
  }

  // Structural BPB check, not a filesystem consistency check. The containing
  // region and supported logical sector size are checked before arithmetic.
  inline std::optional<Fat> fat(std::span<const std::uint8_t> b,
                                std::uint64_t available) {
    if (!signature(b) || !(b[0] == 0xe9 || (b[0] == 0xeb && b[2] == 0x90))) {
      return std::nullopt;
    }
    const auto bytes = little(b.subspan(11, 2));
    const std::uint32_t spc = b[13];
    const auto reserved = little(b.subspan(14, 2));
    const auto roots = little(b.subspan(17, 2));
    const auto small_total = little(b.subspan(19, 2));
    const auto total = small_total ? small_total : little(b.subspan(32, 4));
    const auto small_fat = little(b.subspan(22, 2));
    const auto fat_size = small_fat ? small_fat : little(b.subspan(36, 4));
    if (bytes != 512 || !spc || (spc & (spc - 1)) || spc > 128 || !reserved ||
        !b[16] || b[16] > 2 || !fat_size || !total || total > available) {
      return std::nullopt;
    }
    const std::uint64_t overhead =
        reserved + std::uint64_t{b[16]} * fat_size + (roots * 32 + 511) / 512;
    if (overhead >= total) {
      return std::nullopt;
    }
    const auto clusters = static_cast<std::uint32_t>((total - overhead) / spc);
    const bool fat32 = clusters >= 65525;
    if (!clusters || clusters > 0x0ffffff5 ||
        (fat32 && (roots || small_fat || small_total ||
                   little(b.subspan(42, 2)) || little(b.subspan(44, 4)) < 2 ||
                   little(b.subspan(44, 4)) > clusters + 1)) ||
        (!fat32 && (!roots || !small_fat))) {
      return std::nullopt;
    }
    const std::uint32_t bits = fat32 ? 32 : clusters < 4085 ? 12 : 16;
    if (std::uint64_t{fat_size} * 512 * 8 / bits <
        std::uint64_t{clusters} + 2) {
      return std::nullopt;
    }
    return Fat{fat32             ? "FAT32"
               : clusters < 4085 ? "FAT12"
                                 : "FAT16",
               total, spc * bytes, clusters};
  }

  struct Partition {
    std::uint8_t type = 0;
    std::uint32_t start = 0;
    std::uint32_t sectors = 0;
  };

  inline std::optional<std::array<Partition, 4>> partitions(
      std::span<const std::uint8_t> b, std::uint64_t available) {
    if (!signature(b)) {
      return std::nullopt;
    }
    std::array<Partition, 4> result{};
    bool any = false;
    for (std::size_t i = 0; i < result.size(); ++i) {
      auto entry = b.subspan(446 + i * 16, 16);
      auto& p = result[i];
      p = {entry[4], little(entry.subspan(8, 4)), little(entry.subspan(12, 4))};
      if (!p.type && !p.start && !p.sectors && !entry[0]) {
        continue;
      }
      if ((entry[0] != 0 && entry[0] != 0x80) || !p.type || !p.start ||
          !p.sectors || std::uint64_t{p.start} + p.sectors > available) {
        return std::nullopt;
      }
      for (std::size_t j = 0; j < i; ++j) {
        const auto& other = result[j];
        if (other.type &&
            p.start < std::uint64_t{other.start} + other.sectors &&
            other.start < std::uint64_t{p.start} + p.sectors) {
          return std::nullopt;
        }
      }
      any = true;
    }
    return any ? std::optional{result} : std::nullopt;
  }

  inline bool exfat(std::span<const std::uint8_t> b) {
    constexpr std::string_view magic = "EXFAT   ";
    return signature(b) &&
           std::equal(magic.begin(), magic.end(), b.begin() + 3);
  }


}  // namespace app::sd
