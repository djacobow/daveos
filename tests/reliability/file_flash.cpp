#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "boot/control.h"
#include "platform/host/flash.h"
#include "support.hpp"
#include "update/engine.h"
#include "util/crc32.h"

namespace {
  namespace core = daveos::core;
  namespace boot = daveos::boot;
  namespace update = daveos::update;
  namespace crc = daveos::util::crc32;
  using FileFlash = daveos::platform::host::FileFlash;

  struct Files {
    char directory[32] = "file-flash-XXXXXX";
    std::filesystem::path path;

    Files() {
      REQUIRE(::mkdtemp(directory) != nullptr);
      path = std::filesystem::path(directory) / "flash.bin";
    }

    ~Files() { std::filesystem::remove_all(directory); }
  };

  std::vector<std::byte> Read(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    REQUIRE(file.good());
    std::vector<std::byte> bytes(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    REQUIRE(file.good());
    return bytes;
  }

  core::Status Finish(boot::Flash driver) {
    auto status = driver.poll(driver.context);
    for (unsigned i = 0; status == core::Status::busy && i < 10; ++i) {
      status = driver.poll(driver.context);
    }
    return status;
  }
}  // namespace

TEST_CASE(
    "File flash enforces geometry, NOR bits, exclusive ownership and "
    "persistence") {
  Files files;
  FileFlash flash;
  const FileFlash::Geometry geometry{0x08000000, 4096, 512};
  auto driver = flash.driver();
  REQUIRE(driver.erase(driver.context, geometry.base) ==
          core::Status::not_running);
  REQUIRE(flash.open(files.path.c_str(), geometry,
                     FileFlash::OpenMode::create) == core::Status::ok);
  REQUIRE(std::filesystem::file_size(files.path) == 4096);
  auto raw = Read(files.path.c_str());
  REQUIRE(std::all_of(raw.begin(), raw.end(),
                      [](auto b) { return b == std::byte{0xff}; }));
  FileFlash other;
  REQUIRE(other.open(files.path.c_str(), geometry) == core::Status::busy);
  REQUIRE(driver.erase(driver.context, geometry.base + 1) ==
          core::Status::invalid_argument);
  REQUIRE(driver.erase(driver.context, geometry.base + 4096) ==
          core::Status::invalid_argument);
  std::array<std::byte, 16> word{};
  REQUIRE(driver.program(driver.context, geometry.base - 16, word) ==
          core::Status::invalid_argument);
  REQUIRE(driver.program(driver.context, geometry.base + 1, word) ==
          core::Status::invalid_argument);
  REQUIRE(driver.program(driver.context, geometry.base,
                         std::span(word).first(15)) ==
          core::Status::invalid_argument);
  REQUIRE(driver.program(driver.context, geometry.base, word) ==
          core::Status::ok);
  REQUIRE(driver.erase(driver.context, geometry.base) == core::Status::busy);
  REQUIRE(Read(files.path.c_str()) ==
          raw);  // Submission alone does not mutate the file.
  REQUIRE(Finish(driver) == core::Status::ok);
  word.fill(std::byte{0xff});
  REQUIRE(driver.program(driver.context, geometry.base, word) ==
          core::Status::invalid_argument);
  flash.close();
  REQUIRE(flash.open(files.path.c_str(), geometry) == core::Status::ok);
  std::array<std::byte, 16> saved;
  REQUIRE(driver.read(driver.context, geometry.base, saved) ==
          core::Status::ok);
  REQUIRE(saved == std::array<std::byte, 16>{});
  REQUIRE(driver.erase(driver.context, geometry.base) == core::Status::ok);
  REQUIRE(driver.poll(driver.context) == core::Status::busy);
  flash.close();  // Cancel a submitted operation before it executes.
  REQUIRE(flash.open(files.path.c_str(), geometry) == core::Status::ok);
  REQUIRE(driver.read(driver.context, geometry.base, saved) ==
          core::Status::ok);
  REQUIRE(saved == std::array<std::byte, 16>{});
  REQUIRE(driver.erase(driver.context, geometry.base) == core::Status::ok);
  REQUIRE(Finish(driver) == core::Status::ok);
  REQUIRE(driver.read(driver.context, geometry.base, saved) ==
          core::Status::ok);
  REQUIRE(saved == word);
  flash.close();
  REQUIRE(flash.open(files.path.c_str(), geometry,
                     FileFlash::OpenMode::create) == core::Status::io_error);
  REQUIRE(Read(files.path.c_str()) ==
          raw);  // Failed creation cannot truncate existing data.
  REQUIRE(flash.open(files.path.c_str(), {geometry.base, 8192, 512}) ==
          core::Status::invalid_argument);
}

TEST_CASE(
    "OTA installs into a file and a new driver boots and confirms its "
    "persisted trial") {
  Files files;
  const FileFlash::Geometry geometry{0, 4096, 512};
  const boot::Layout layout{{1024, 3072}, {0, 512}, 1024, 512, 16, 1, 1};
  const auto package = Read(REFERENCE_PACKAGE);
  const auto expected = Read(REFERENCE_IMAGE);
  testing::Fake clock;
  {
    FileFlash flash(
        &clock, [](void* p) { return static_cast<testing::Fake*>(p)->now(); });
    REQUIRE(flash.open(files.path.c_str(), geometry,
                       FileFlash::OpenMode::create) == core::Status::ok);
    auto driver = flash.driver();
    clock.advance(123);
    REQUIRE(driver.now(driver.context) == 123);
    std::array<std::byte, 16> original{};
    REQUIRE(driver.program(driver.context, layout.slots[0], original) ==
            core::Status::ok);
    REQUIRE(Finish(driver) == core::Status::ok);
    boot::Snapshot factory;
    factory.counter = 1;
    factory.images[0] = {
        1, 16, crc::calculate(original), boot::ImageState::confirmed, 1, 1};
    boot::Journal journal(driver, layout);
    REQUIRE(journal.commit(factory, 1000000, true) == core::Status::ok);
    update::Engine engine(driver, layout, 0);
    engine.enable(true);
    REQUIRE(engine.begin(std::span(package).first(128)) == core::Status::ok);
    for (unsigned i = 0; i < 20000 && engine.active(); ++i) {
      engine.tick();
      clock.advance(1000);
      if (engine.ready()) {
        auto bytes = std::span(package).subspan(128 + engine.next_offset());
        bytes = bytes.first(std::min<std::size_t>(63, bytes.size()));
        REQUIRE(engine.chunk(engine.next_offset(), bytes,
                             crc::calculate(bytes)) == core::Status::ok);
      }
    }
    REQUIRE(engine.state() == update::Engine::State::done);
    auto disk = Read(files.path.c_str());
    REQUIRE(std::equal(expected.begin(), expected.end(),
                       disk.begin() + layout.slots[1]));
    REQUIRE(std::equal(original.begin(), original.end(),
                       disk.begin() + layout.slots[0]));
  }
  {
    FileFlash reopened;
    REQUIRE(reopened.open(files.path.c_str(), geometry) == core::Status::ok);
    boot::Control bootloader(reopened.driver(), layout);
    const auto selected = bootloader.select();
    REQUIRE(selected.status == core::Status::ok);
    REQUIRE(selected.slot == 1);
    REQUIRE(selected.trial);
    REQUIRE(bootloader.confirm_image(1) == core::Status::ok);
  }
  {
    FileFlash reopened;
    REQUIRE(reopened.open(files.path.c_str(), geometry) == core::Status::ok);
    boot::Control bootloader(reopened.driver(), layout);
    const auto selected = bootloader.select();
    REQUIRE(selected.status == core::Status::ok);
    REQUIRE(selected.slot == 1);
    REQUIRE_FALSE(selected.trial);
  }
}
