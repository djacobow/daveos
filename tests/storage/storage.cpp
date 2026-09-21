#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <memory>

#include "platform/host/block_device.h"
#include "storage/module.hpp"
#include "support.hpp"

namespace {
  namespace storage = daveos::storage;
  namespace core = daveos::core;
  using Sector = std::array<std::uint8_t, 512>;

  void Put(Sector& b, std::size_t offset, std::uint32_t value,
           std::size_t count = 4) {
    for (std::size_t i = 0; i < count; ++i) {
      b[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
  }

  // Sparse file containing a FAT16 volume with a deliberately fragmented
  // 700-byte file (clusters 2 -> 4), an empty subdirectory and an LFN entry.
  // We construct known bytes instead of relying on host mkfs/mtools versions.
  struct Image {
    char directory[40] = "storage-XXXXXX";
    std::filesystem::path path;
    std::fstream file;
    daveos::platform::host::FileBlockDevice device;
    std::array<std::uint8_t, 700> contents{};

    Image() {
      REQUIRE(::mkdtemp(directory));
      path = std::filesystem::path(directory) / "card.img";
      file.open(path, std::ios::in | std::ios::out | std::ios::binary |
                          std::ios::trunc);
      file.seekp(8192 * 512 - 1);
      file.put('\0');
      Sector boot{};
      boot[0] = 0xeb;
      boot[2] = 0x90;
      Put(boot, 11, 512, 2);
      boot[13] = 1;
      Put(boot, 14, 1, 2);
      boot[16] = 1;
      Put(boot, 17, 32, 2);
      Put(boot, 19, 8192, 2);
      boot[21] = 0xf8;
      Put(boot, 22, 32, 2);
      boot[510] = 0x55;
      boot[511] = 0xaa;
      Write(0, boot);
      Sector fat{};
      Put(fat, 0, 0xfff8, 2);
      Put(fat, 2, 0xffff, 2);
      Put(fat, 4, 4, 2);
      Put(fat, 6, 0xffff, 2);
      Put(fat, 8, 0xffff, 2);
      Write(1, fat);
      Sector root{};
      constexpr std::string_view short_name = "HELLO   TXT";
      std::copy(short_name.begin(), short_name.end(), root.begin() + 32);
      root[32 + 11] = AM_ARC;
      Put(root, 32 + 26, 2, 2);
      Put(root, 32 + 28, contents.size());
      root[0] = 0x41;
      root[11] = 0x0f;
      std::uint8_t checksum = 0;
      for (auto c : short_name) {
        checksum = static_cast<std::uint8_t>(((checksum & 1) ? 128 : 0) +
                                             (checksum >> 1) + c);
      }
      root[13] = checksum;
      constexpr std::array offsets{1,  3,  5,  7,  9,  14, 16,
                                   18, 20, 22, 24, 28, 30};
      constexpr std::string_view long_name = "Long name.txt";
      for (std::size_t i = 0; i < offsets.size(); ++i) {
        Put(root, offsets[i], static_cast<std::uint8_t>(long_name[i]), 2);
      }
      constexpr std::string_view dir_name = "EMPTY      ";
      std::copy(dir_name.begin(), dir_name.end(), root.begin() + 64);
      root[64 + 11] = AM_DIR;
      Put(root, 64 + 26, 3, 2);
      Write(33, root);
      for (std::size_t i = 0; i < contents.size(); ++i) {
        contents[i] = static_cast<std::uint8_t>(i * 7);
      }
      Sector data{};
      std::copy_n(contents.begin(), 512, data.begin());
      Write(35, data);
      data.fill(0);
      std::copy(contents.begin() + 512, contents.end(), data.begin());
      Write(37, data);
      file.flush();
      REQUIRE(device.open(path.c_str()));
    }

    ~Image() {
      device.close();
      file.close();
      std::filesystem::remove_all(directory);
    }

    void Write(std::uint32_t sector, const Sector& bytes) {
      file.seekp(std::uint64_t{sector} * 512);
      file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
  };
}  // namespace

TEST_CASE(
    "FatFs mounts a file image and reads fragmented long-named files without "
    "modifying it") {
  Image image;
  storage::Volume volume(image.device.device());
  REQUIRE(volume.attach() == FR_OK);
  REQUIRE(volume.mount() == FR_OK);
  CHECK_FALSE(image.device.close());  // Mounted media cannot be replaced.
  REQUIRE(volume.open_directory("/") == FR_OK);
  FILINFO entry{};
  REQUIRE(volume.next(entry) == FR_OK);
  CHECK(std::string_view{entry.fname} == "Long name.txt");
  CHECK(entry.fsize == 700);
  REQUIRE(volume.next(entry) == FR_OK);
  CHECK(std::string_view{entry.fname} == "EMPTY");
  CHECK(entry.fattrib & AM_DIR);
  REQUIRE(volume.next(entry) == FR_OK);
  CHECK(entry.fname[0] == 0);
  REQUIRE(volume.close() == FR_OK);
  REQUIRE(volume.open("Long name.txt") == FR_OK);
  std::array<std::uint8_t, 1024> bytes{};
  std::uint32_t count = 0;
  REQUIRE(volume.read(bytes, count) == FR_OK);
  CHECK(count == image.contents.size());
  CHECK(
      std::equal(image.contents.begin(), image.contents.end(), bytes.begin()));
  REQUIRE(volume.read(bytes, count) == FR_OK);
  CHECK(count == 0);
  REQUIRE(volume.close() == FR_OK);
  REQUIRE(volume.open("HELLO.TXT", 500) == FR_OK);
  REQUIRE(volume.read(std::span{bytes}.first(100), count) == FR_OK);
  CHECK(count == 100);
  CHECK(std::equal(bytes.begin(), bytes.begin() + count,
                   image.contents.begin() + 500));
  REQUIRE(volume.close() == FR_OK);
  CHECK(volume.open("HELLO.TXT", 701) == FR_INVALID_PARAMETER);
  CHECK(volume.open("missing") == FR_NO_FILE);
  CHECK(volume.open("1:/HELLO.TXT") == FR_INVALID_NAME);
  REQUIRE(volume.open_directory("/EMPTY") == FR_OK);
  REQUIRE(volume.next(entry) == FR_OK);
  CHECK(entry.fname[0] == 0);
  CHECK(volume.unmount() == FR_OK);
  CHECK(volume.open("HELLO.TXT") == FR_NOT_READY);
  CHECK(image.device.close());
}

TEST_CASE(
    "Volume rejects nested access and releases registry and media leases") {
  Image image;
  auto device = image.device.device();

  struct Hook {
    storage::BlockDevice device;
    storage::Volume* volume = nullptr;
    bool called = false;
  } hook{device};

  device.context = &hook;
  device.ready = [](void* p) {
    auto& h = *static_cast<Hook*>(p);
    return h.device.ready(h.device.context);
  };
  device.sectors = [](void* p) {
    auto& h = *static_cast<Hook*>(p);
    return h.device.sectors(h.device.context);
  };
  device.acquire = [](void* p) {
    auto& h = *static_cast<Hook*>(p);
    return h.device.acquire(h.device.context);
  };
  device.release = [](void* p) {
    auto& h = *static_cast<Hook*>(p);
    h.device.release(h.device.context);
  };
  device.read = [](void* p, std::uint32_t sector,
                   std::span<std::uint8_t> bytes) {
    auto& h = *static_cast<Hook*>(p);
    CHECK(h.volume->mount() == FR_LOCKED);
    CHECK(h.volume->unmount() == FR_LOCKED);
    CHECK(h.volume->open("HELLO.TXT") == FR_LOCKED);
    h.called = true;
    return h.device.read(h.device.context, sector, bytes);
  };
  {
    storage::Volume volume(device);
    hook.volume = &volume;
    REQUIRE(volume.attach() == FR_OK);
    REQUIRE(volume.mount() == FR_OK);
    CHECK(hook.called);
    storage::Volume duplicate(image.device.device());
    REQUIRE(duplicate.attach() == FR_OK);
    CHECK(duplicate.mount() == FR_LOCKED);
  }
  CHECK(image.device.close());
  std::array<std::unique_ptr<storage::Volume>, FF_VOLUMES + 1> volumes;
  for (std::size_t i = 0; i < volumes.size(); ++i) {
    volumes[i] = std::make_unique<storage::Volume>(image.device.device());
    CHECK(volumes[i]->attach() ==
          (i < FF_VOLUMES ? FR_OK : FR_TOO_MANY_OPEN_FILES));
  }
  volumes[0].reset();
  CHECK(volumes.back()->attach() == FR_OK);
}

TEST_CASE(
    "FatFs propagates absent media, read failure and invalid boot sectors") {
  Image image;
  storage::Volume volume(image.device.device());
  REQUIRE(volume.attach() == FR_OK);
  Sector zeros{};
  image.Write(0, zeros);
  image.file.flush();
  CHECK(volume.mount() == FR_NO_FILESYSTEM);
  CHECK(image.device.close());
  CHECK(volume.mount() == FR_NOT_READY);
  REQUIRE(image.device.open(image.path.c_str()));
  auto device = image.device.device();
  device.read = [](void*, std::uint32_t, std::span<std::uint8_t>) {
    return false;
  };
  storage::Volume failing(device);
  REQUIRE(failing.attach() == FR_OK);
  CHECK(failing.mount() == FR_DISK_ERR);
  CHECK(image.device.close());
}

TEST_CASE(
    "filesystem module copies command arguments and returns between output "
    "chunks") {
  Image image;
  testing::Fake platform;
  storage::Module<testing::Event> fs(image.device.device());
  testing::TestModule control;
  testing::Sink sink;
  auto logger =
      core::make_logger(platform, core::SubscriberList{sink.subscriber()});
  auto scheduler = core::make_scheduler<testing::Event>(
      platform, core::ModuleList{&fs, &control}, logger);
  REQUIRE(scheduler.init() == core::Status::ok);
  control.first_action = [&] {
    CHECK(fs.Mount() == core::Status::ok);
    CHECK(fs.List({}) == core::Status::busy);
  };
  control.second_action = [&] {
    std::string path = "Long name.txt";
    CHECK(fs.Read(path, {}, 700) == core::Status::ok);
    path.assign("discarded");
  };
  control.third_action = [&] {
    CHECK(fs.Unmount() == core::Status::ok);
    scheduler.timer(
        10000, +[] { testing::timer_action(); });
  };
  testing::timer_action = [&] { scheduler.stop(); };
  scheduler.schedule(control, &testing::TestModule::first, 0);
  scheduler.schedule(control, &testing::TestModule::second, 10000);
  scheduler.schedule(control, &testing::TestModule::third, 100000);
  REQUIRE(scheduler.run() == core::Status::ok);
#if DAVEOS_LOGGING
  CHECK(std::none_of(
      sink.records.begin(), sink.records.end(),
      [](const auto& r) { return r.severity == core::Level::error; }));
  CHECK(std::count_if(sink.records.begin(), sink.records.end(),
                      [](const auto& r) {
                        return r.module == "fs" && r.message.size() >= 10 &&
                               r.message.starts_with("00000") &&
                               r.message.substr(8, 2) == "  ";
                      }) == 44);
  CHECK(
      std::any_of(sink.records.begin(), sink.records.end(), [](const auto& r) {
        return r.message == "Filesystem request complete";
      }));
#endif
  CHECK(image.device.close());
}
