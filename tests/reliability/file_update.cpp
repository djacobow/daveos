#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "boot/control.h"
#include "platform/host/flash.h"
#include "support.hpp"
#include "update/file.h"
#include "update/protocol.h"
#include "util/wire.h"

namespace {
  namespace update = daveos::update;
  namespace boot = daveos::boot;
  namespace crc = daveos::util::crc32;
  using Status = daveos::core::Status;
  using FileFlash = daveos::platform::host::FileFlash;

  std::vector<std::byte> Read(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    std::vector<std::byte> b(static_cast<std::size_t>(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(b.data()), b.size());
    return b;
  }

  struct Fixture {
    char directory[40] = "file-update-XXXXXX";
    std::filesystem::path package, flash_path;
    std::ifstream input;
    std::uint64_t time = 0;
    FileFlash flash{
        this, [](void* p) { return static_cast<Fixture*>(p)->time += 100; }};
    boot::Layout layout{{1024, 3072}, {0, 512}, 1024, 512, 16, 1, 1};
    update::Engine engine{flash.driver(), layout, 0};
    bool reserved = false, mounted = true, writable = false;
    bool fail_open = false, fail_seek = false, fail_close = false;
    std::size_t maximum_read = 1024, reads = 0, fail_read = SIZE_MAX;
    unsigned releases = 0, closes = 0;

    update::FileSource source() {
      return {this,
              [](void* p) {
                auto& f = *static_cast<Fixture*>(p);
                if (f.reserved) {
                  return Status::busy;
                }
                if (!f.mounted) {
                  return Status::not_running;
                }
                if (f.writable) {
                  return Status::rejected;
                }
                f.reserved = true;
                return Status::ok;
              },
              [](void* p) {
                auto& f = *static_cast<Fixture*>(p);
                REQUIRE(f.reserved);
                f.reserved = false;
                ++f.releases;
              },
              [](void* p, std::string_view) {
                auto& f = *static_cast<Fixture*>(p);
                if (f.fail_open) {
                  return Status::not_found;
                }
                f.input.clear();
                f.input.open(f.package, std::ios::binary);
                return f.input ? Status::ok : Status::io_error;
              },
              [](void* p, std::span<std::byte> b, std::uint32_t& count) {
                auto& f = *static_cast<Fixture*>(p);
                count = 0;
                if (f.reads++ == f.fail_read) {
                  return Status::io_error;
                }
                f.input.read(reinterpret_cast<char*>(b.data()),
                             std::min(b.size(), f.maximum_read));
                count = f.input.gcount();
                return f.input.bad() ? Status::io_error : Status::ok;
              },
              [](void* p, std::uint32_t offset) {
                auto& f = *static_cast<Fixture*>(p);
                if (f.fail_seek) {
                  return Status::io_error;
                }
                f.input.clear();
                f.input.seekg(offset);
                return f.input ? Status::ok : Status::io_error;
              },
              [](void* p) {
                auto& f = *static_cast<Fixture*>(p);
                f.input.close();
                ++f.closes;
                return f.fail_close ? Status::io_error : Status::ok;
              }};
    }

    update::FileUpdate file{engine, source()};

    explicit Fixture(bool large = false)
        : layout(
              large
                  ? boot::Layout{{4096, 16384}, {0, 512}, 8192, 512, 16, 1, 1}
                  : boot::Layout{{1024, 3072}, {0, 512}, 1024, 512, 16, 1, 1}) {
      REQUIRE(mkdtemp(directory));
      package = std::filesystem::path(directory) / "image.ota";
      flash_path = std::filesystem::path(directory) / "flash.bin";
      std::filesystem::copy_file(large ? LARGE_PACKAGE : REFERENCE_PACKAGE,
                                 package);
      REQUIRE(flash.open(flash_path.c_str(), {0, large ? 32768u : 4096u, 512},
                         FileFlash::OpenMode::create) == Status::ok);
      boot::Snapshot state;
      state.counter = 1;
      state.images[0] = {1, 16, 0, boot::ImageState::confirmed, 1, 1};
      boot::Journal journal(flash.driver(), layout);
      REQUIRE(journal.commit(state, 1000000, true) == Status::ok);
    }

    ~Fixture() {
      input.close();
      flash.close();
      std::filesystem::remove_all(directory);
    }

    void Tick() {
      engine.tick();
      file.tick();
      time += 1000;
    }

    void Prepare() {
      REQUIRE(file.prepare("/image.ota") == Status::ok);
      for (unsigned i = 0; i < 20000 && file.busy() &&
                           file.state() != update::FileUpdate::State::prepared;
           ++i) {
        Tick();
      }
    }

    void Finish() {
      for (unsigned i = 0; i < 20000 && file.busy(); ++i) {
        Tick();
      }
      REQUIRE_FALSE(file.busy());
      REQUIRE_FALSE(reserved);
      REQUIRE_FALSE(engine.reserved());
    }

    void Replace(std::vector<std::byte> bytes) {
      std::ofstream f(package, std::ios::binary | std::ios::trunc);
      f.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }
  };
}  // namespace

TEST_CASE(
    "SD package validation preserves flash and installation uses file-backed "
    "flash") {
  Fixture f;
  f.maximum_read = GENERATE(1u, 7u, 1024u);
  const auto before = Read(f.flash_path);
  f.Prepare();
  REQUIRE(f.file.state() == update::FileUpdate::State::prepared);
  CHECK(Read(f.flash_path) == before);
  CHECK_FALSE(f.engine.enabled());
  CHECK(f.file.prepare("other") == Status::busy);
  const auto reads = f.reads;
  f.time += 1000000000;
  for (unsigned i = 0; i < 100; ++i) {
    f.Tick();
  }
  CHECK(f.reads == reads);
  CHECK(f.reserved);
  auto bytes = Read(f.package);
  CHECK(f.engine.begin(std::span(bytes).first(128)) == Status::busy);
  REQUIRE(f.file.install() == Status::ok);
  CHECK(f.file.install() == Status::busy);
  f.Finish();
  REQUIRE(f.file.status() == Status::ok);
  CHECK(f.closes == 1);
  CHECK(f.releases == 1);
  CHECK_FALSE(f.engine.enabled());
  auto expected = Read(REFERENCE_IMAGE);
  auto flashed = Read(f.flash_path);
  CHECK(std::equal(expected.begin(), expected.end(), flashed.begin() + 3072));
  CHECK(std::equal(before.begin() + 1024, before.begin() + 2048,
                   flashed.begin() + 1024));
  boot::Journal journal(f.flash.driver(), f.layout);
  boot::Snapshot state;
  REQUIRE(journal.load(state) == Status::ok);
  CHECK(state.images[1].state == boot::ImageState::pending);
  CHECK(f.file.install() == Status::not_running);
}

TEST_CASE(
    "SD malformed packages fail before flash changes and release "
    "reservations") {
  Fixture f;
  auto bytes = Read(f.package);
  const auto kind = GENERATE(0, 1, 2, 3, 4, 5);
  if (kind == 0) {
    bytes.resize(60);
  }
  if (kind == 1) {
    bytes[124] ^= std::byte{1};
  }
  if (kind == 2) {
    bytes.back() ^= std::byte{1};
  }
  if (kind == 3) {
    bytes.pop_back();
  }
  if (kind == 4) {
    bytes.push_back(std::byte{0});
  }
  if (kind == 5) {
    bytes[128] = std::byte{1};
  }
  f.Replace(bytes);
  const auto before = Read(f.flash_path);
  f.Prepare();
  f.Finish();
  CHECK(f.file.state() == update::FileUpdate::State::failed);
  CHECK(Read(f.flash_path) == before);
  CHECK(f.closes == 1);
  CHECK(f.releases == 1);
}

TEST_CASE(
    "SD admissions and I/O failures clean up without retaining a prepared "
    "file") {
  Fixture f;
  const auto kind = GENERATE(0, 1, 2, 3, 4, 5, 6, 7);
  const auto before = Read(f.flash_path);
  if (kind == 0) {
    f.mounted = false;
    CHECK(f.file.prepare("p") == Status::not_running);
  }
  if (kind == 1) {
    f.writable = true;
    CHECK(f.file.prepare("p") == Status::rejected);
  }
  if (kind == 2) {
    f.fail_open = true;
    f.Prepare();
    f.Finish();
    CHECK(f.closes == 0);
  }
  if (kind == 3) {
    f.fail_read = 1;
    f.Prepare();
    f.Finish();
  }
  if (kind == 4) {
    f.Prepare();
    f.fail_seek = true;
    REQUIRE(f.file.install() == Status::ok);
    f.Finish();
  }
  if (kind == 5) {
    f.Prepare();
    f.file.cancel();
    f.Finish();
  }
  if (kind == 6) {
    REQUIRE(f.file.prepare("p") == Status::ok);
    f.file.cancel();
    f.Finish();
    CHECK(f.closes == 0);
  }
  if (kind == 7) {
    f.Prepare();
    f.fail_close = true;
    f.file.cancel();
    f.Finish();
  }
  CHECK_FALSE(f.reserved);
  CHECK_FALSE(f.engine.reserved());
  CHECK(Read(f.flash_path) == before);
}

TEST_CASE(
    "SD installation read failure aborts flash work and leaves no pending "
    "candidate") {
  Fixture f;
  f.Prepare();
  REQUIRE(f.file.state() == update::FileUpdate::State::prepared);
  f.fail_read =
      f.reads +
      1;  // Recheck header succeeds, first installation payload read fails.
  REQUIRE(f.file.install() == Status::ok);
  f.Finish();
  CHECK(f.file.status() == Status::io_error);
  CHECK(f.closes == 1);
  boot::Journal journal(f.flash.driver(), f.layout);
  boot::Snapshot state;
  REQUIRE(journal.load(state) == Status::ok);
  CHECK(state.images[1].state != boot::ImageState::pending);
}

TEST_CASE("SD install rechecks header, EOF and close before committing") {
  Fixture f;
  f.Prepare();
  REQUIRE(f.file.state() == update::FileUpdate::State::prepared);
  const auto before = Read(f.flash_path);
  const auto kind = GENERATE(0, 1, 2);
  if (kind == 0) {
    auto b = Read(f.package);
    b[12] ^= std::byte{1};
    f.Replace(b);
  } else if (kind == 1) {
    auto b = Read(f.package);
    b.push_back(std::byte{0});
    f.Replace(b);
  } else {
    f.fail_close = true;
  }
  REQUIRE(f.file.install() == Status::ok);
  f.Finish();
  CHECK(f.file.state() == update::FileUpdate::State::failed);
  if (kind == 0) {
    CHECK(Read(f.flash_path) == before);
  }
  boot::Journal journal(f.flash.driver(), f.layout);
  boot::Snapshot state;
  REQUIRE(journal.load(state) == Status::ok);
  CHECK(state.images[1].state != boot::ImageState::pending);
  CHECK(f.closes == 1);
  CHECK(f.releases == 1);
}

TEST_CASE(
    "SD cancellation waits for engine cleanup and permits a fresh "
    "preparation") {
  Fixture f;
  f.Prepare();
  REQUIRE(f.file.install() == Status::ok);
  const auto phase = GENERATE(
      update::Engine::State::invalidating, update::Engine::State::writing,
      update::Engine::State::verifying, update::Engine::State::committing);
  for (unsigned i = 0; i < 20000 && f.engine.state() != phase; ++i) {
    f.Tick();
  }
  REQUIRE(f.engine.state() == phase);
  f.file.cancel();
  f.engine.enable(false);
  f.Finish();
  CHECK(f.file.status() == Status::rejected);
  boot::Journal journal(f.flash.driver(), f.layout);
  boot::Snapshot state;
  REQUIRE(journal.load(state) == Status::ok);
  CHECK(state.images[1].state != boot::ImageState::pending);
  f.Prepare();
  REQUIRE(f.file.state() == update::FileUpdate::State::prepared);
  f.file.cancel();
  f.Finish();
  CHECK(f.releases == 2);
}

TEST_CASE(
    "Network ownership and protocol commands cannot disturb prepared SD "
    "updates") {
  Fixture f;
  auto b = Read(f.package);
  f.engine.enable(true);
  REQUIRE(f.engine.begin(std::span(b).first(128)) == Status::ok);
  CHECK(f.file.prepare("image.ota") == Status::busy);
  CHECK_FALSE(f.reserved);
  f.engine.abort();
  for (unsigned i = 0; i < 20000 && f.engine.active(); ++i) {
    f.engine.tick();
    f.time += 1000;
  }
  REQUIRE_FALSE(f.engine.active());
  f.Prepare();
  REQUIRE(f.file.state() == update::FileUpdate::State::prepared);
  update::Protocol protocol(f.engine);
  for (auto op : {1u, 2u, 3u, 4u, 5u}) {
    std::array<std::byte, 16> header{};
    daveos::util::wire::write32(header, 0, update::kProtocolMagic);
    daveos::util::wire::write32(header, 4, 1);
    daveos::util::wire::write32(header, 8, op);
    protocol.reset();
    protocol.tick();
    std::size_t used = 0;
    for (unsigned i = 0; i < 10 && protocol.reply().empty(); ++i) {
      used += protocol.tick(std::span(header).subspan(used));
    }
    REQUIRE_FALSE(protocol.reply().empty());
    CHECK(daveos::util::wire::read32(protocol.reply(), 16) ==
          static_cast<std::uint32_t>(Status::busy));
    CHECK(daveos::util::wire::read32(protocol.reply(), 24) == 0);
    CHECK(f.reserved);
    CHECK(f.file.state() == update::FileUpdate::State::prepared);
  }
  f.file.cancel();
  f.Finish();
}

TEST_CASE(
    "SD update streams multiple blocks and relocation boundaries with partial "
    "file reads") {
  Fixture f(true);
  f.maximum_read = GENERATE(1u, 7u, 511u, 1024u);
  f.Prepare();
  REQUIRE(f.file.state() == update::FileUpdate::State::prepared);
  REQUIRE(f.file.install() == Status::ok);
  f.Finish();
  REQUIRE(f.file.status() == Status::ok);
  const auto expected = Read(LARGE_IMAGE);
  const auto flashed = Read(f.flash_path);
  CHECK(std::equal(expected.begin(), expected.end(),
                   flashed.begin() + f.layout.slots[1]));
}
