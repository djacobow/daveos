#pragma once

#include <array>
#include <cstring>
#include <string_view>

#include "crc32.h"
#include "version.h"
#include "wire.h"

namespace daveos::util::fault {


  enum class Kind : std::uint32_t {
    hard_fault,
    memory_fault,
    bus_fault,
    usage_fault,
    watchdog,
    initialization,
    boot
  };

  struct Data {
    Kind kind = Kind::hard_fault;
    Version version{};
    std::uint64_t installation = 0;
    std::uint32_t status = 0;
    // r0-r3, r12, lr, pc, xpsr; populated only when the frame is readable.
    std::array<std::uint32_t, 8> frame{};
    std::uint32_t cfsr = 0, hfsr = 0, mmfar = 0, bfar = 0;
    std::uint32_t exc_return = 0, stack = 0;
    bool frame_valid = false;
    char check[32]{}, module[32]{}, task[32]{};
    std::uint64_t expected = 0, completed = 0;
  };

  // Fixed byte layout shared across images/toolchains; first word is magic,
  // with no native struct padding or flash string pointers in retained RAM.
  struct alignas(16) Record {
    std::array<std::byte, 256> bytes;
  };

  inline void copy_name(char (&out)[32], std::string_view name) {
    auto count = name.size() < 31 ? name.size() : 31;
    std::memcpy(out, name.data(), count);
    out[count] = '\0';
  }

  void save(Record& record, const Data& data);
  bool read(const Record& record, Data& data);
  void clear(Record& record);


}  // namespace daveos::util::fault
