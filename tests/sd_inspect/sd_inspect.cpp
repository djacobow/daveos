#include "examples/stm32_console/sd_inspect.h"

#include "support.hpp"

namespace {
  namespace sd = app::sd;
  using Sector = std::array<std::uint8_t, 512>;

  void Put(Sector& s, std::size_t offset, std::uint32_t value,
           std::size_t count = 4) {
    for (std::size_t i = 0; i < count; ++i) {
      s[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
  }

  Sector Boot(std::uint32_t clusters) {
    Sector b{};
    b[0] = 0xeb;
    b[2] = 0x90;
    b[510] = 0x55;
    b[511] = 0xaa;
    Put(b, 11, 512, 2);
    b[13] = 1;
    b[16] = 2;
    const bool fat32 = clusters >= 65525;
    const std::uint32_t fat = ((clusters + 2) * (fat32             ? 32
                                                 : clusters < 4085 ? 12
                                                                   : 16) +
                               4095) /
                              4096;
    Put(b, 14, 32, 2);
    Put(b, 17, fat32 ? 0 : 512, 2);
    Put(b, fat32 ? 36 : 22, fat, fat32 ? 4 : 2);
    Put(b, 32, clusters + 32 + 2 * fat + (fat32 ? 0 : 32));
    if (fat32) {
      Put(b, 44, 2);
    }
    return b;
  }

  Sector Mbr() {
    Sector b{};
    b[510] = 0x55;
    b[511] = 0xaa;
    b[450] = 0x0c;
    Put(b, 454, 2048);
    Put(b, 458, 1000000);
    return b;
  }
}  // namespace

TEST_CASE(
    "SD data CRC verifies known vectors and rejects corrupt wire packets") {
  Sector block;
  block.fill(0xff);
  REQUIRE(sd::crc16(block) == 0x7fa1);
  const std::array<std::uint8_t, 9> check{'1', '2', '3', '4', '5',
                                          '6', '7', '8', '9'};
  REQUIRE(sd::crc16(check) == 0x31c3);
  std::array<std::uint8_t, 540> wire;
  wire.fill(0xff);
  wire[7] = 0;
  wire[20] = 0xfe;
  wire[533] = 0x7f;
  wire[534] = 0xa1;
  auto parsed = sd::data(wire, 512, 13);
  REQUIRE(parsed);
  REQUIRE(std::equal(parsed->begin(), parsed->end(), block.begin()));
  REQUIRE_FALSE(sd::data(wire, 512, 12));
  REQUIRE_FALSE(sd::data(std::span{wire}.first(534), 512, 13));
  wire[30] ^= 1;
  REQUIRE_FALSE(sd::data(wire, 512, 13));
  wire[30] ^= 1;
  wire[8] = 0x0b;  // Data error token: do not scan past it.
  REQUIRE_FALSE(sd::data(wire, 512, 13));
  wire[8] = 0xff;
  wire[7] = 4;  // Illegal-command R1.
  REQUIRE_FALSE(sd::data(wire, 512, 13));
  wire[7] = 0xff;
  wire[8] = 0;  // R1 outside the eight-byte response window.
  REQUIRE_FALSE(sd::data(wire, 512, 13));
  REQUIRE_FALSE(sd::data({}, 512, 13));
}

TEST_CASE("SD CSD v2 reports capacity without 32-bit overflow") {
  std::array<std::uint8_t, 16> csd{};
  csd[0] = 0x40;
  csd[3] = 0x32;  // 25 MHz.
  csd[5] = 9;
  csd[8] = 0xff;
  csd[9] = 0xff;
  REQUIRE(sd::card(csd)->sectors == 67108864);
  REQUIRE(sd::card(csd)->maximum_hz == 25000000);
  csd[7] = 0x3f;
  REQUIRE(sd::card(csd)->sectors == (std::uint64_t{1} << 32));
  csd[0] = 0;
  REQUIRE_FALSE(sd::card(csd));
  csd[0] = 0x40;
  csd[3] = 0xff;
  REQUIRE_FALSE(sd::card(csd));
  REQUIRE_FALSE(sd::card(std::span{csd}.first(15)));
}

TEST_CASE("FAT identification uses cluster counts and bounded BPB arithmetic") {
  for (auto count : {4084u, 4085u, 65524u, 65525u, 1000000u}) {
    auto b = Boot(count);
    const auto f = sd::fat(b, 2000000);
    REQUIRE(f);
    REQUIRE(f->clusters == count);
    REQUIRE(std::string_view{f->name} == (count < 4085    ? "FAT12"
                                          : count < 65525 ? "FAT16"
                                                          : "FAT32"));
    REQUIRE(f->cluster_bytes == 512);
    REQUIRE_FALSE(sd::fat(b, f->sectors - 1));
  }
  auto b = Boot(1000000);
  b[13] = 3;
  REQUIRE_FALSE(sd::fat(b, 2000000));
  b = Boot(1000000);
  Put(b, 36, 0xffffffff);
  REQUIRE_FALSE(sd::fat(b, 2000000));
  b = Boot(1000000);
  Put(b, 36, 1);  // FAT cannot hold declared cluster count.
  REQUIRE_FALSE(sd::fat(b, 2000000));
  b = Boot(1000000);
  Put(b, 44, 1000002);
  REQUIRE_FALSE(sd::fat(b, 2000000));
  b = Boot(1000000);
  b[510] = 0;
  REQUIRE_FALSE(sd::fat(b, 2000000));
  b = Boot(1000000);
  Put(b, 11, 4096, 2);
  REQUIRE_FALSE(sd::fat(b, 2000000));
  REQUIRE_FALSE(sd::fat({}, 2000000));
}

TEST_CASE("MBR inspection bounds every partition and rejects overlap") {
  auto b = Mbr();
  auto result = sd::partitions(b, 2000000);
  REQUIRE(result);
  REQUIRE((*result)[0].start == 2048);
  REQUIRE((*result)[0].sectors == 1000000);
  REQUIRE((*result)[0].type == 0x0c);
  REQUIRE_FALSE(sd::partitions(b, 1000000));
  Put(b, 454, 0xfffffff0);
  REQUIRE_FALSE(sd::partitions(b, 0xffffffff));
  b = Mbr();
  b[466] = 0x0b;
  Put(b, 470, 3000);
  Put(b, 474, 500);
  REQUIRE_FALSE(sd::partitions(b, 2000000));
  Put(b, 470, 1100000);
  REQUIRE(sd::partitions(b, 2000000));
  b[462] = 0x42;
  REQUIRE_FALSE(sd::partitions(b, 2000000));
  REQUIRE_FALSE(sd::partitions({}, 2000000));
}

TEST_CASE("exFAT detection is a signature hint only") {
  Sector b{};
  b[510] = 0x55;
  b[511] = 0xaa;
  constexpr std::string_view name = "EXFAT   ";
  std::copy(name.begin(), name.end(), b.begin() + 3);
  REQUIRE(sd::exfat(b));
  REQUIRE_FALSE(sd::fat(b, 2000000));
  b[3] = 'X';
  REQUIRE_FALSE(sd::exfat(b));
  REQUIRE_FALSE(sd::exfat({}));
}
