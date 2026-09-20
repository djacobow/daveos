#include "otp/flash.h"

#include <unistd.h>

#include <filesystem>
#include <set>

#include "otp/store.h"
#include "platform/host/flash.h"
#include "support.hpp"

namespace {
  namespace otp = daveos::otp;
  namespace boot = daveos::boot;
  using Status = daveos::core::Status;
  using FileFlash = daveos::platform::host::FileFlash;
  constexpr std::uint32_t kBase = 0x08100000;

  struct Fixture {
    char directory[32] = "/tmp/daveos-flashotp-XXXXXX";
    std::string path;
    FileFlash file;
    boot::Flash raw = file.driver();
    std::set<std::uint32_t> writes, bad_reads;
    std::optional<std::uint32_t> fail_at;
    std::uint32_t prefix = 1, calls = 0, polls = 0, erases = 0;
    bool fail_pending = false, busy = false, hung = false;
    std::uint64_t now = 0;
    std::array<std::byte, 16> torn{};

    Fixture() {
      REQUIRE(::mkdtemp(directory));
      path = std::string(directory) + "/flash.bin";
      Open(FileFlash::OpenMode::create);
    }

    ~Fixture() {
      file.close();
      std::filesystem::remove_all(directory);
    }

    void Open(FileFlash::OpenMode mode = FileFlash::OpenMode::existing) {
      REQUIRE(file.open(path.c_str(), {kBase - 8192, 3 * 8192, 8192}, mode) ==
              Status::ok);
    }

    boot::Flash driver() {
      return {this,
              [](void* p, std::uint32_t at, std::span<std::byte> out) {
                auto& self = *static_cast<Fixture*>(p);
                if (self.bad_reads.contains(at)) {
                  return Status::io_error;
                }
                return self.raw.read(self.raw.context, at, out);
              },
              [](void* p, std::uint32_t) {
                ++static_cast<Fixture*>(p)->erases;
                return Status::rejected;
              },
              [](void* p, std::uint32_t at, std::span<const std::byte> bytes) {
                auto& self = *static_cast<Fixture*>(p);
                if (self.busy) {
                  return Status::busy;
                }
                REQUIRE(at >= kBase);
                REQUIRE(at + 16 <= kBase + otp::FlashOtp::kSize);
                REQUIRE(self.writes.insert(at).second);
                ++self.calls;
                self.fail_pending = self.fail_at == self.calls;
                if (self.fail_pending) {
                  self.torn.fill(std::byte{0xff});
                  std::copy_n(bytes.begin(), self.prefix, self.torn.begin());
                  bytes = self.torn;
                }
                return self.raw.program(self.raw.context, at, bytes);
              },
              [](void* p) {
                auto& self = *static_cast<Fixture*>(p);
                ++self.polls;
                if (self.hung) {
                  return Status::busy;
                }
                auto status = self.raw.poll(self.raw.context);
                if (status == Status::ok && self.fail_pending) {
                  self.fail_pending = false;
                  return Status::io_error;
                }
                return status;
              },
              [](void* p) { return static_cast<Fixture*>(p)->now += 1000; }};
    }

    void CheckOutside() {
      std::array<std::byte, 8192> bytes{};
      for (auto at : {kBase - 8192, kBase + 8192}) {
        REQUIRE(raw.read(raw.context, at, bytes) == Status::ok);
        CHECK(std::all_of(bytes.begin(), bytes.end(),
                          [](auto b) { return b == std::byte{0xff}; }));
      }
      CHECK(erases == 0);
    }
  };
}  // namespace

TEST_CASE("Flash OTP persists records, locks and exhaustion without erasing") {
  Fixture f;
  {
    otp::FlashOtp backend(f.driver(), kBase);
    otp::Store store(backend.driver());
    REQUIRE(store.init() == Status::ok);
    for (std::uint32_t i = 0; i < otp::kBlockCount; ++i) {
      REQUIRE(store.set_serial(std::to_string(i)) == Status::ok);
    }
    CHECK(store.set_serial("31") == Status::ok);
    CHECK(store.set_serial("new") == Status::full);
    CHECK(store.snapshot().remaining == 0);
  }
  f.file.close();
  f.Open();
  otp::FlashOtp backend(f.driver(), kBase);
  otp::Store store(backend.driver());
  REQUIRE(store.init() == Status::ok);
  CHECK(store.serial() == "31");
  CHECK(store.snapshot().serial_locked);
  CHECK(store.snapshot().consumed == 32);
  f.CheckOutside();
}

TEST_CASE(
    "Flash OTP recovers torn claim, body, commit and lock writes after "
    "reopen") {
  for (std::uint32_t step = 1; step <= 7; ++step) {
    for (std::uint32_t prefix = 1; prefix <= 16; ++prefix) {
      CAPTURE(step, prefix);
      Fixture f;
      const std::string next(56, 'x');
      {
        otp::FlashOtp backend(f.driver(), kBase);
        otp::Store store(backend.driver());
        REQUIRE(store.init() == Status::ok);
        REQUIRE(store.set_serial("old") == Status::ok);
        f.fail_at = f.calls + step;
        f.prefix = prefix;
        REQUIRE(store.set_serial(next) == Status::io_error);
        CHECK(store.serial() == (step == 7 ? next : "old"));
        CHECK(store.snapshot().consumed == 2);
      }
      f.file.close();
      f.Open();
      f.fail_at.reset();
      otp::FlashOtp backend(f.driver(), kBase);
      otp::Store store(backend.driver());
      REQUIRE(store.init() == Status::ok);
      const bool committed = step == 7 || (step == 6 && prefix == 16);
      CHECK(store.serial() == (committed ? next : "old"));
      CHECK(store.snapshot().consumed == 2);
      REQUIRE(store.set_serial(next) == Status::ok);
      CHECK(store.snapshot().consumed == (committed ? 2 : 3));
      CHECK(store.snapshot().serial_locked);
      f.CheckOutside();
    }
  }
}

TEST_CASE(
    "Flash OTP lock retries consume ten cells then fail without rewriting") {
  Fixture f;
  otp::FlashOtp backend(f.driver(), kBase);
  otp::Store store(backend.driver());
  REQUIRE(store.init() == Status::ok);
  // A short serial requires claim + one body word + commit before lock.
  f.fail_at = 4;
  REQUIRE(store.set_serial("ABC") == Status::io_error);
  REQUIRE(store.serial() == "ABC");
  for (std::uint32_t i = 1; i < 10; ++i) {
    f.fail_at = f.calls + 1;
    CHECK(store.set_serial("ABC") == Status::io_error);
  }
  const auto calls = f.calls;
  f.fail_at.reset();
  CHECK(store.set_serial("ABC") == Status::io_error);
  CHECK(f.calls == calls);
  CHECK(store.snapshot().consumed == 1);
  CHECK(store.snapshot().phase == otp::Phase::lock);
  CHECK_FALSE(store.snapshot().serial_locked);
  f.file.close();
  f.Open();
  otp::FlashOtp reopened(f.driver(), kBase);
  otp::Store recovered(reopened.driver());
  REQUIRE(recovered.init() == Status::ok);
  CHECK(recovered.serial() == "ABC");
  CHECK(recovered.set_serial("ABC") == Status::io_error);
  f.CheckOutside();
}

TEST_CASE(
    "Flash OTP never polls a rejected operation and bounds its own waits") {
  Fixture f;
  otp::FlashOtp backend(f.driver(), kBase);
  otp::Store store(backend.driver());
  REQUIRE(store.init() == Status::ok);
  SECTION("another operation owns flash") {
    std::array<std::byte, 16> other{};
    REQUIRE(f.raw.program(f.raw.context, kBase - 8192, other) == Status::ok);
    f.busy = true;
    CHECK(store.set_serial("ABC") == Status::busy);
    CHECK(store.snapshot().consumed == 0);
    CHECK(f.polls == 0);
    auto result = f.raw.poll(f.raw.context);
    if (result == Status::busy) {
      result = f.raw.poll(f.raw.context);
    }
    REQUIRE(result == Status::ok);
    f.busy = false;
    REQUIRE(store.set_serial("ABC") == Status::ok);
  }
  SECTION("own operation times out") {
    f.hung = true;
    CHECK(store.set_serial("ABC") == Status::timeout);
    CHECK(store.snapshot().consumed == 1);
    auto calls = f.calls;
    CHECK(store.set_serial("different") == Status::timeout);
    CHECK(f.calls == calls);
    CHECK(f.polls <= 100);
  }
}

TEST_CASE("Flash OTP ECC consumes bad blocks and skips damaged lock cells") {
  Fixture f;
  {
    otp::FlashOtp backend(f.driver(), kBase);
    otp::Store store(backend.driver());
    REQUIRE(store.init() == Status::ok);
    f.fail_at = 4;
    REQUIRE(store.set_serial("ABC") == Status::io_error);
  }
  f.bad_reads.insert(kBase + 96);  // Damaged lock cell retains valid record.
  f.bad_reads.insert(kBase + 256 + 16);  // Damaged body consumes next block.
  f.fail_at.reset();
  otp::FlashOtp backend(f.driver(), kBase);
  otp::Store store(backend.driver());
  REQUIRE(store.init() == Status::ok);
  CHECK(store.serial() == "ABC");
  CHECK(store.snapshot().unreadable == 1);
  REQUIRE(store.set_serial("ABC") == Status::ok);
  CHECK(store.snapshot().serial_locked);
  REQUIRE(store.set_serial("new") == Status::ok);
  CHECK(store.snapshot().serial_block == 2);
  f.CheckOutside();
}
