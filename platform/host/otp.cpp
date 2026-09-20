#include "otp.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

namespace daveos::platform::host {


  namespace {
    using Status = otp::Status;

    std::array<std::byte, 64> Header() {
      std::array<std::byte, 64> header{};
      constexpr std::string_view magic = "DVOTP001";
      std::copy_n(reinterpret_cast<const std::byte*>(magic.data()),
                  magic.size(), header.data());
      header[8] = std::byte{1};
      header[12] = std::byte{32};
      header[16] = std::byte{64};
      header[20] = std::byte{128};
      auto crc = util::crc32::calculate(std::span(header).first(60));
      for (std::size_t i = 0; i < 4; ++i) {
        header[60 + i] = std::byte((crc >> (i * 8)) & 0xff);
      }
      return header;
    }
  }  // namespace

  FileOtp::~FileOtp() { close(); }

  void FileOtp::close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  bool FileOtp::Read(std::size_t offset, std::span<std::byte> data) const {
    while (!data.empty()) {
      auto count =
          ::pread(fd_, data.data(), data.size(), static_cast<off_t>(offset));
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        return false;
      }
      offset += std::size_t(count);
      data = data.subspan(std::size_t(count));
    }
    return true;
  }

  bool FileOtp::Write(std::size_t offset, std::span<const std::byte> data) {
    while (!data.empty()) {
      auto count =
          ::pwrite(fd_, data.data(), data.size(), static_cast<off_t>(offset));
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        return false;
      }
      offset += std::size_t(count);
      data = data.subspan(std::size_t(count));
    }
    std::int32_t result;
    do {
      result = ::fsync(fd_);
    } while (result < 0 && errno == EINTR);
    return result == 0;
  }

  bool FileOtp::Persist(std::size_t offset, std::span<const std::byte> data) {
    if (failure_ == 0) {
      failure_.reset();
      return false;
    }
    attempted_ = true;
    if (!Write(offset, data)) {
      return false;
    }
    if (failure_ && --*failure_ == 0) {
      failure_.reset();
      return false;
    }
    return true;
  }

  Status FileOtp::Load(std::uint32_t index, Image& image) const {
    if (fd_ < 0) {
      return Status::not_running;
    }
    if (index >= otp::kBlockCount) {
      return Status::invalid_argument;
    }
    if (!Read(Offset(index), image)) {
      return Status::io_error;
    }
    for (std::size_t i = 0; i < 32; ++i) {
      if (std::to_integer<std::uint8_t>(image[64 + i]) >
          std::uint8_t(Word::unreadable)) {
        return Status::incompatible;
      }
      if (image[64 + i] == std::byte(Word::virgin) &&
          (image[2 * i] != std::byte{0xff} ||
           image[2 * i + 1] != std::byte{0xff})) {
        return Status::incompatible;
      }
    }
    if (image[96] > std::byte{1} ||
        !std::all_of(image.begin() + 97, image.end(),
                     [](std::byte b) { return b == std::byte{0}; })) {
      return Status::incompatible;
    }
    return Status::ok;
  }

  Status FileOtp::Init() {
    if (fd_ >= 0) {
      return Status::already_initialized;
    }
    if (!path_ || !*path_) {
      return Status::invalid_argument;
    }
    fd_ = ::open(
        path_,
        O_RDWR | O_CLOEXEC | (mode_ == OpenMode::create ? O_CREAT | O_EXCL : 0),
        0600);
    if (fd_ < 0) {
      return Status::io_error;
    }
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      const auto status = errno == EWOULDBLOCK || errno == EAGAIN
                              ? Status::busy
                              : Status::io_error;
      close();
      return status;
    }
    auto fail = [&](Status status) {
      close();
      return status;
    };

    struct stat info {};

    if (::fstat(fd_, &info) != 0 || !S_ISREG(info.st_mode)) {
      return fail(Status::io_error);
    }
    constexpr auto kFileSize = kHeaderSize + otp::kBlockCount * kImageSize;
    if (mode_ == OpenMode::create) {
      if (!Write(0, Header())) {
        return fail(Status::io_error);
      }
      Image image{};
      std::fill_n(image.begin(), otp::kRecordSize, std::byte{0xff});
      for (std::uint32_t i = 0; i < otp::kBlockCount; ++i) {
        if (!Write(Offset(i), image)) {
          return fail(Status::io_error);
        }
      }
    } else if (info.st_size != kFileSize) {
      return fail(Status::incompatible);
    }
    std::array<std::byte, kHeaderSize> header{};
    if (!Read(0, header)) {
      return fail(Status::io_error);
    }
    if (header != Header()) {
      return fail(Status::incompatible);
    }
    for (std::uint32_t i = 0; i < otp::kBlockCount; ++i) {
      Image image{};
      auto status = Load(i, image);
      if (status != Status::ok) {
        return fail(status);
      }
    }
    return Status::ok;
  }

  Status FileOtp::Inspect(std::uint32_t index, otp::Block& block) {
    Image image{};
    auto status = Load(index, image);
    if (status != Status::ok) {
      return status;
    }
    block = {};
    std::copy_n(image.begin(), otp::kRecordSize, block.record.bytes.begin());
    block.locked = image[96] == std::byte{1};
    if (block.locked) {
      block.storage = otp::Storage::consumed;
    }
    for (std::size_t i = 0; i < 32; ++i) {
      auto state = Word(std::to_integer<std::uint8_t>(image[64 + i]));
      if (state == Word::attempted || state == Word::unreadable) {
        block.storage = otp::Storage::unreadable;
        break;
      }
      if (state == Word::programmed) {
        block.storage = otp::Storage::consumed;
      }
    }
    return Status::ok;
  }

  otp::ProgramResult FileOtp::Program(std::uint32_t index,
                                      const otp::Record& record) {
    otp::Block block;
    auto status = Inspect(index, block);
    if (status != Status::ok) {
      return {status, false};
    }
    if (block.storage != otp::Storage::unused || block.locked) {
      return {Status::rejected, false};
    }
    attempted_ = false;
    // Length (halfword 1) is written first, and type (halfword 0) last.
    for (std::uint32_t step = 0; step < 32; ++step) {
      auto word = (step + 1) % 32;
      std::byte state = std::byte(Word::attempted);
      if (!Persist(Offset(index) + 64 + word, std::span(&state, 1)) ||
          !Persist(Offset(index) + word * 2,
                   std::span(record.bytes).subspan(word * 2, 2))) {
        return {Status::io_error, attempted_};
      }
      state = std::byte(Word::programmed);
      if (!Persist(Offset(index) + 64 + word, std::span(&state, 1))) {
        return {Status::io_error, attempted_};
      }
    }
    return {Status::ok, true};
  }

  Status FileOtp::Lock(std::uint32_t index) {
    otp::Block block;
    auto status = Inspect(index, block);
    if (status != Status::ok || block.locked) {
      return status;
    }
    const std::byte locked{1};
    return Persist(Offset(index) + 96, std::span(&locked, 1))
               ? Status::ok
               : Status::io_error;
  }

  Status FileOtp::inject_read_error(std::uint32_t block,
                                    std::uint32_t halfword) {
    if (fd_ < 0) {
      return Status::not_running;
    }
    if (block >= otp::kBlockCount || halfword >= 32) {
      return Status::invalid_argument;
    }
    const std::byte state = std::byte(Word::unreadable);
    return Write(Offset(block) + 64 + halfword, std::span(&state, 1))
               ? Status::ok
               : Status::io_error;
  }

  otp::Driver FileOtp::driver() {
    return {this,
            "file",
            otp::kBlockCount,
            otp::kRecordSize,
            [](void* p) { return static_cast<FileOtp*>(p)->Init(); },
            [](void* p, std::uint32_t index, otp::Block& block) {
              return static_cast<FileOtp*>(p)->Inspect(index, block);
            },
            [](void* p, std::uint32_t index, const otp::Record& record) {
              return static_cast<FileOtp*>(p)->Program(index, record);
            },
            [](void* p, std::uint32_t index) {
              return static_cast<FileOtp*>(p)->Lock(index);
            }};
  }


}  // namespace daveos::platform::host
