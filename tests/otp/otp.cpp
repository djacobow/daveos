#include "platform/host/otp.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "core/command/command.hpp"
#include "otp/module.hpp"
#include "platform/stm32h5/otp_ecc.h"
#include "support.hpp"

namespace otp = daveos::otp;
namespace core = daveos::core;
using FileOtp = daveos::platform::host::FileOtp;
using Status = core::Status;

namespace {
  struct File {
    std::array<char, 32> directory{};
    std::string path;

    File() {
      std::string_view pattern = "/tmp/daveos-otp-XXXXXX";
      std::copy(pattern.begin(), pattern.end(), directory.begin());
      REQUIRE(::mkdtemp(directory.data()) != nullptr);
      path = std::string(directory.data()) + "/otp.bin";
    }

    ~File() { std::filesystem::remove_all(directory.data()); }
  };

  struct Memory {
    std::array<otp::Block, otp::kBlockCount> blocks{};
    Status program_status = Status::ok, lock_status = Status::ok;
    bool attempted = true, corrupt = false, verify_failure = false;
    std::uint32_t programs = 0, locks = 0, inspections = 0;

    otp::Driver driver() {
      return {this,
              "memory",
              otp::kBlockCount,
              otp::kRecordSize,
              [](void*) { return Status::ok; },
              [](void* p, std::uint32_t i, otp::Block& b) {
                auto& self = *static_cast<Memory*>(p);
                ++self.inspections;
                if (self.verify_failure && self.programs) {
                  return Status::io_error;
                }
                b = self.blocks[i];
                return Status::ok;
              },
              [](void* p, std::uint32_t i, const otp::Record& r) {
                auto& self = *static_cast<Memory*>(p);
                ++self.programs;
                if (self.attempted) {
                  self.blocks[i] = {r, otp::Storage::consumed, false};
                  if (self.corrupt) {
                    self.blocks[i].record.bytes[8] ^= std::byte{1};
                  }
                }
                return otp::ProgramResult{self.program_status, self.attempted};
              },
              [](void* p, std::uint32_t i) {
                auto& self = *static_cast<Memory*>(p);
                ++self.locks;
                if (self.lock_status == Status::ok) {
                  self.blocks[i].locked = true;
                }
                return self.lock_status;
              }};
    }
  };

  void FixCrc(otp::Record& record) {
    auto crc = daveos::util::crc32::calculate(std::span(record.bytes).first(4));
    crc = daveos::util::crc32::update(crc, std::span(record.bytes).subspan(8));
    for (std::size_t i = 0; i < 4; ++i) {
      record.bytes[4 + i] = std::byte((crc >> (8 * i)) & 0xff);
    }
  }
}  // namespace

TEST_CASE("OTP record validates CRC, padding, length and printable serial") {
  auto r = otp::serial_record("ABC 123");
  REQUIRE(r.valid());
  CHECK(r.type() == 1);
  CHECK(r.size() == 7);
  CHECK(r.payload() == "ABC 123");
  CHECK(r.bytes[15] == std::byte{0xff});
  // Independent Python binascii.crc32 vector over header-without-CRC + payload.
  CHECK(std::span(r.bytes).subspan(4, 4)[0] == std::byte{0x2e});
  CHECK(r.bytes[5] == std::byte{0x96});
  CHECK(r.bytes[6] == std::byte{0x51});
  CHECK(r.bytes[7] == std::byte{0x62});
  r.bytes[15] = std::byte{0};
  FixCrc(r);
  CHECK_FALSE(r.valid());
  r = otp::serial_record("ABC");
  r.bytes[8] ^= std::byte{1};
  CHECK_FALSE(r.valid());
  r.bytes[2] = std::byte{57};
  CHECK_FALSE(r.valid());
  CHECK(r.payload().empty());
  CHECK(otp::valid_serial(std::string(56, ' ')));
  CHECK_FALSE(otp::valid_serial(std::string(57, 'A')));
  CHECK_FALSE(otp::valid_serial(""));
  for (std::uint32_t value = 0; value < 256; ++value) {
    const char c = static_cast<char>(value);
    CHECK(otp::valid_serial(std::string_view(&c, 1)) ==
          (value >= 0x20 && value <= 0x7e));
  }
}

TEST_CASE("OTP cache uses latest valid record and never fills earlier gaps") {
  Memory memory;
  memory.blocks[0] = {otp::serial_record("old"), otp::Storage::consumed, true};
  auto unknown = otp::serial_record("future");
  unknown.bytes[0] = std::byte{2};
  FixCrc(unknown);
  memory.blocks[1] = {unknown, otp::Storage::consumed, true};
  memory.blocks[2].storage = otp::Storage::unreadable;
  memory.blocks[3] = {otp::serial_record("new"), otp::Storage::consumed, false};
  memory.blocks[5] = {otp::serial_record("bad"), otp::Storage::consumed, true};
  memory.blocks[5].record.bytes[8] ^= std::byte{1};
  otp::Store store(memory.driver());
  CHECK_FALSE(store.serial());
  CHECK(store.set_serial("x") == Status::not_running);
  REQUIRE(store.init() == Status::ok);
  CHECK(store.init() == Status::already_initialized);
  CHECK(store.serial() == "new");
  auto s = store.snapshot();
  CHECK(s.consumed == 5);
  CHECK(s.remaining == 26);
  CHECK(s.gaps == 1);
  CHECK(s.invalid == 1);
  CHECK(s.unknown == 1);
  CHECK(s.unreadable == 1);
  CHECK_FALSE(s.serial_locked);
  CHECK(memory.locks == 0);
  const auto reads = memory.inspections;
  CHECK(store.serial() == "new");
  CHECK(memory.inspections == reads);
  REQUIRE(store.set_serial("next") == Status::ok);
  CHECK(store.snapshot().serial_block == 6);
}

TEST_CASE(
    "OTP failures preserve cache, consume attempted rows and retry locks "
    "only") {
  Memory memory;
  otp::Store store(memory.driver());
  REQUIRE(store.init() == Status::ok);
  REQUIRE(store.set_serial("old") == Status::ok);
  SECTION("busy before touching storage") {
    memory.program_status = Status::busy;
    memory.attempted = false;
    CHECK(store.set_serial("new") == Status::busy);
    CHECK(store.snapshot().consumed == 1);
  }
  SECTION("program failure") {
    memory.program_status = Status::io_error;
    CHECK(store.set_serial("new") == Status::io_error);
    CHECK(store.snapshot().consumed == 2);
  }
  SECTION("readback mismatch") {
    memory.corrupt = true;
    CHECK(store.set_serial("new") == Status::checksum_error);
    CHECK(store.snapshot().phase == otp::Phase::verify);
  }
  SECTION("readback failure") {
    memory.verify_failure = true;
    CHECK(store.set_serial("new") == Status::io_error);
    CHECK(store.snapshot().unreadable == 1);
  }
  SECTION("lock failure publishes verified value and retries without rewrite") {
    memory.lock_status = Status::io_error;
    REQUIRE(store.set_serial("new") == Status::io_error);
    CHECK(store.serial() == "new");
    const auto* data = store.serial()->data();
    memory.lock_status = Status::ok;
    REQUIRE(store.set_serial("new") == Status::ok);
    CHECK(store.serial()->data() == data);
    CHECK(memory.programs == 2);
    CHECK(store.snapshot().serial_locked);
    return;
  }
  CHECK(store.serial() == "old");
}

TEST_CASE(
    "OTP full storage still accepts identical serial, invalid input never "
    "writes") {
  Memory memory;
  otp::Store store(memory.driver());
  REQUIRE(store.init() == Status::ok);
  for (const auto& text : {std::string(), std::string(57, 'a'),
                           std::string("\n"), std::string("a\0b", 3)}) {
    CHECK(store.set_serial(text) == Status::invalid_argument);
  }
  CHECK(memory.programs == 0);
  for (std::uint32_t i = 0; i < otp::kBlockCount; ++i) {
    REQUIRE(store.set_serial(std::to_string(i)) == Status::ok);
  }
  CHECK(store.snapshot().remaining == 0);
  CHECK(store.set_serial("different") == Status::full);
  CHECK(store.set_serial("31") == Status::ok);
  CHECK(memory.programs == 32);
  CHECK(memory.locks == 32);
}

TEST_CASE(
    "FileOtp persists every interrupted halfword, commit and lock boundary") {
  for (std::uint32_t step = 0; step <= 98; ++step) {
    CAPTURE(step);
    File file;
    {
      FileOtp backend(file.path.c_str(), FileOtp::OpenMode::create);
      otp::Store store(backend.driver());
      REQUIRE(store.init() == Status::ok);
      REQUIRE(store.set_serial("old") == Status::ok);
      backend.fail_after(step);
      CHECK(store.set_serial("new") ==
            (step <= 97 ? Status::io_error : Status::ok));
      CHECK(store.serial() == (step >= 97 ? "new" : "old"));
      CHECK(store.snapshot().consumed == (step == 0 ? 1 : 2));
    }
    {
      FileOtp backend(file.path.c_str());
      otp::Store store(backend.driver());
      REQUIRE(store.init() == Status::ok);
      CHECK(store.serial() == (step >= 96 ? "new" : "old"));
      CHECK(store.snapshot().consumed == (step == 0 ? 1 : 2));
      REQUIRE(store.set_serial("new") == Status::ok);
      CHECK(store.snapshot().consumed == (step > 0 && step < 96 ? 3 : 2));
      CHECK(store.snapshot().serial_locked);
    }
  }
}

TEST_CASE(
    "FileOtp distinguishes programmed all-ones from unused, rejects "
    "rewriting") {
  File file;
  {
    FileOtp backend(file.path.c_str(), FileOtp::OpenMode::create);
    auto driver = backend.driver();
    REQUIRE(driver.init(driver.context) == Status::ok);
    otp::Record erased;
    erased.bytes.fill(std::byte{0xff});
    REQUIRE(driver.program(driver.context, 0, erased).status == Status::ok);
    CHECK_FALSE(driver.program(driver.context, 0, erased).attempted);
    REQUIRE(backend.inject_read_error(1, 0) == Status::ok);
  }
  FileOtp backend(file.path.c_str());
  otp::Store store(backend.driver());
  REQUIRE(store.init() == Status::ok);
  CHECK(store.snapshot().invalid == 1);
  CHECK(store.snapshot().unreadable == 1);
  CHECK(store.snapshot().consumed == 2);
  REQUIRE(store.set_serial("ABC 123") == Status::ok);
  CHECK(store.snapshot().serial_block == 2);
}

TEST_CASE(
    "FileOtp rejects concurrent ownership and malformed files without "
    "truncating") {
  File file;
  {
    FileOtp backend(file.path.c_str(), FileOtp::OpenMode::create);
    otp::Store store(backend.driver());
    REQUIRE(store.init() == Status::ok);
    FileOtp other(file.path.c_str());
    otp::Store second(other.driver());
    CHECK(second.init() == Status::busy);
    CHECK(second.init() == Status::initialization_failed);
    CHECK_FALSE(second.ready());
    FileOtp create(file.path.c_str(), FileOtp::OpenMode::create);
    otp::Store third(create.driver());
    CHECK(third.init() == Status::io_error);
  }
  const auto size = std::filesystem::file_size(file.path);
  std::fstream corrupt(file.path,
                       std::ios::binary | std::ios::in | std::ios::out);
  corrupt.put('!');
  corrupt.close();
  FileOtp backend(file.path.c_str());
  otp::Store store(backend.driver());
  CHECK(store.init() == Status::incompatible);
  CHECK(std::filesystem::file_size(file.path) == size);
}

TEST_CASE(
    "OTP serial commands require matching confirmation before any mutation") {
  Memory memory;
  otp::Store store(memory.driver());
  otp::Module<testing::Event> module(store);
  testing::TestModule input;
  testing::Fake platform;
  auto modules = core::ModuleList{&input, &module};
  auto scheduler = core::make_scheduler<testing::Event>(platform, modules);
  core::CommandDispatcher dispatcher(modules, scheduler);
  input.initializer = [&](core::InitStage stage) {
    if (stage == core::InitStage::stage2) {
      CHECK(store.ready());
    }
    return Status::ok;
  };
  input.first_action = [&] {
    CHECK(dispatcher.dispatch("otp serial") == Status::ok);
    CHECK(dispatcher.dispatch("otp status") == Status::ok);
    for (auto line :
         {"otp serial set", "otp serial set x", "otp serial set x y",
          "otp serial set x x x", "otp serial nope x x"}) {
      CHECK(dispatcher.dispatch(line) == Status::invalid_argument);
    }
    CHECK(memory.programs == 0);
    CHECK(memory.locks == 0);
    CHECK(dispatcher.dispatch(R"(otp serial set "ABC 123" "ABC 123")") ==
          Status::ok);
    CHECK(store.serial() == "ABC 123");
    CHECK(dispatcher.dispatch(R"(otp serial set "ABC 123" "ABC 123")") ==
          Status::ok);
    CHECK(memory.programs == 1);
    CHECK(memory.locks == 1);
    scheduler.stop();
  };
  REQUIRE(scheduler.schedule<&testing::TestModule::first>(
              input, std::chrono::microseconds{0}) == Status::ok);
  CHECK(scheduler.run() == Status::ok);
}

TEST_CASE("OTP rejects incompatible geometry before touching the backend") {
  Memory memory;
  auto driver = memory.driver();
  driver.blocks = 31;
  otp::Store store(driver);
  CHECK(store.init() == Status::incompatible);
  CHECK(memory.inspections == 0);
  CHECK_FALSE(store.ready());
  CHECK_FALSE(store.serial());
  CHECK(store.set_serial("ABC") == Status::not_running);
}

TEST_CASE("OTP aliasing cache views are staged before replacing the value") {
  Memory memory;
  otp::Store store(memory.driver());
  REQUIRE(store.init() == Status::ok);
  REQUIRE(store.set_serial("ABC 123") == Status::ok);
  REQUIRE(store.set_serial(store.serial()->substr(4)) == Status::ok);
  CHECK(store.serial() == "123");
}

TEST_CASE(
    "FileOtp lock failure before mutation retries the existing unlocked row") {
  File file;
  {
    FileOtp backend(file.path.c_str(), FileOtp::OpenMode::create);
    auto driver = backend.driver();
    REQUIRE(driver.init(driver.context) == Status::ok);
    REQUIRE(
        driver.program(driver.context, 0, otp::serial_record("ABC")).status ==
        Status::ok);
  }
  FileOtp backend(file.path.c_str());
  otp::Store store(backend.driver());
  REQUIRE(store.init() == Status::ok);
  REQUIRE(store.serial() == "ABC");
  REQUIRE_FALSE(store.snapshot().serial_locked);
  backend.fail_after(0);
  REQUIRE(store.set_serial("ABC") == Status::io_error);
  CHECK(store.snapshot().consumed == 1);
  REQUIRE(store.set_serial("ABC") == Status::ok);
  CHECK(store.snapshot().serial_locked);
  CHECK(store.snapshot().consumed == 1);
}

TEST_CASE(
    "OTP commands preserve maximum escaped serials and reject lock-only "
    "mismatch") {
  Memory memory;
  memory.blocks[0] = {otp::serial_record("ABC"), otp::Storage::consumed, false};
  otp::Store store(memory.driver());
  otp::Module<testing::Event> module(store);
  testing::TestModule input;
  testing::Fake platform;
  auto modules = core::ModuleList{&input, &module};
  auto scheduler = core::make_scheduler<testing::Event>(platform, modules);
  core::CommandDispatcher dispatcher(modules, scheduler);
  input.first_action = [&] {
    CHECK(dispatcher.dispatch("otp serial set ABC abc") ==
          Status::invalid_argument);
    CHECK(memory.locks == 0);
    std::string quoted = "\"";
    for (std::size_t i = 0; i < otp::kPayloadSize; ++i) {
      quoted += "\\\"";
    }
    quoted += '"';
    REQUIRE(dispatcher.dispatch("otp serial set " + quoted + " " + quoted) ==
            Status::ok);
    CHECK(store.serial() == std::string(56, '"'));
    REQUIRE(dispatcher.dispatch(R"(otp serial set "  abc  " "  abc  ")") ==
            Status::ok);
    CHECK(store.serial() == "  abc  ");
    scheduler.stop();
  };
  REQUIRE(scheduler.schedule<&testing::TestModule::first>(
              input, std::chrono::microseconds{0}) == Status::ok);
  CHECK(scheduler.run() == Status::ok);
}

TEST_CASE("OTP module reports cached serial and backend diagnostics") {
  Memory memory;
  memory.blocks[0] = {otp::serial_record("ABC"), otp::Storage::consumed, false};
  memory.blocks[1].storage = otp::Storage::unreadable;
  otp::Store store(memory.driver());
  otp::Module<testing::Event> module(store);
  testing::TestModule input;
  testing::Fake platform;
  testing::Sink sink;
  auto logger =
      core::make_logger(platform, core::SubscriberList{sink.subscriber()});
  auto modules = core::ModuleList{&input, &module};
  auto scheduler =
      core::make_scheduler<testing::Event>(platform, modules, logger);
  core::CommandDispatcher dispatcher(modules, scheduler);
  input.first_action = [&] {
    CHECK(dispatcher.dispatch("otp serial") == Status::ok);
    CHECK(dispatcher.dispatch("otp status") == Status::ok);
    scheduler.stop();
  };
  REQUIRE(scheduler.schedule<&testing::TestModule::first>(
              input, std::chrono::microseconds{0}) == Status::ok);
  REQUIRE(scheduler.run() == Status::ok);
  while (logger.dispatch()) {
  }
#if DAVEOS_LOGGING
  auto contains = [&](std::string_view message) {
    return std::any_of(
        sink.records.begin(), sink.records.end(),
        [&](const auto& record) { return record.message == message; });
  };
  CHECK(contains("serial: ABC"));
  CHECK(contains("backend memory, ready yes, used 2, free 30, gaps 0"));
  CHECK(contains("serial block 0, locked no"));
  CHECK(contains("last error io_error during scan at block 1"));
#else
  CHECK(sink.records.empty());
#endif
}

TEST_CASE(
    "H563 ECC guards match region bank and address, never all-ones alone") {
  namespace hw = daveos::platform::stm32h5::detail;
  for (std::uint32_t i = 0; i < 1024; ++i) {
    auto address = hw::kOtpBase + i * 2;
    auto flags = hw::kEccd | hw::kOtpFlag | (0x600 + i / 2);
    CHECK(hw::matches(hw::ReadRegion::otp, address, 2, flags));
    CHECK_FALSE(hw::matches(hw::ReadRegion::none, address, 2, flags));
    CHECK_FALSE(
        hw::matches(hw::ReadRegion::otp, address, 2, flags ^ hw::kOtpFlag));
    CHECK_FALSE(
        hw::matches(hw::ReadRegion::otp, address, 2, flags ^ hw::kEccd));
    CHECK_FALSE(hw::matches(hw::ReadRegion::otp, address, 2, flags ^ 1));
    CHECK_FALSE(
        hw::matches(hw::ReadRegion::otp, address, 2, flags | hw::kBankFlag));
  }
  CHECK(hw::matches(hw::ReadRegion::flash, 0x08100004, 4,
                    hw::kEccd | hw::kBankFlag));
  CHECK_FALSE(hw::matches(hw::ReadRegion::flash, 0x08000000, 16,
                          hw::kEccd | hw::kBankFlag));
  CHECK_FALSE(hw::matches(hw::ReadRegion::flash, 0x08000000, 16,
                          hw::kEccd | hw::kOtpFlag));
  CHECK_FALSE(
      hw::matches(hw::ReadRegion::flash, 0x08000000, 16, hw::kEccd | 1));
  CHECK(hw::classify(0xffff, false, 0xffff, false) == hw::Cell::programmed);
  CHECK(hw::classify(0xffff, true, 0xffff, false) == hw::Cell::virgin);
  CHECK(hw::classify(0xffff, true, 0x00ff, false) == hw::Cell::unreadable);
  CHECK(hw::classify(0x00ff, true, 0xffff, false) == hw::Cell::unreadable);
  CHECK(hw::classify(0xffff, true, 0xffff, true) == hw::Cell::unreadable);
}
