#include "flash.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>

namespace daveos::platform::host {


  namespace {
    using Status = core::Status;
  }

  FileFlash::~FileFlash() { close(); }

  core::Status FileFlash::open(const char* path, Geometry geometry,
                               OpenMode mode) {
    if (fd_ >= 0) {
      return Status::already_initialized;
    }
    if (!path || !geometry.size || !geometry.sector_size ||
        geometry.sector_size % 16 || geometry.size % geometry.sector_size ||
        geometry.base % geometry.sector_size ||
        std::uint64_t(geometry.base) + geometry.size > UINT64_C(0x100000000)) {
      return Status::invalid_argument;
    }
    fd_ = ::open(
        path,
        O_RDWR | O_CLOEXEC | (mode == OpenMode::create ? O_CREAT | O_EXCL : 0),
        0600);
    if (fd_ < 0) {
      return Status::io_error;
    }
    geometry_ = geometry;
    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
      close();
      return Status::busy;
    }

    struct stat info {};

    if (::fstat(fd_, &info) != 0 || !S_ISREG(info.st_mode)) {
      close();
      return Status::io_error;
    }
    if (mode == OpenMode::existing && info.st_size != geometry.size) {
      close();
      return Status::invalid_argument;
    }
    if (mode == OpenMode::create) {
      std::array<std::byte, 4096> erased;
      erased.fill(std::byte{0xff});
      for (std::uint32_t offset = 0; offset < geometry.size;) {
        auto bytes = std::span(erased).first(
            std::min<std::size_t>(erased.size(), geometry.size - offset));
        if (!Write(offset, bytes)) {
          close();
          return Status::io_error;
        }
        offset += bytes.size();
      }
      if (::fsync(fd_) != 0) {
        close();
        return Status::io_error;
      }
    }
    // Process any cancelled request through the same state-transition path.
    Poll();
    return Status::ok;
  }

  void FileFlash::close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    closed_ = true;
    requested_ = false;
  }

  bool FileFlash::Range(std::uint32_t address, std::size_t size) const {
    return fd_ >= 0 && address >= geometry_.base &&
           address - geometry_.base <= geometry_.size &&
           size <= geometry_.size - (address - geometry_.base);
  }

  core::Status FileFlash::Read(std::uint32_t address,
                               std::span<std::byte> bytes) {
    if (fd_ < 0) {
      return Status::not_running;
    }
    if (!Range(address, bytes.size())) {
      return Status::invalid_argument;
    }
    if (requested_ || cs != State::idle) {
      return Status::busy;
    }
    std::size_t at = 0;
    while (at < bytes.size()) {
      const auto n = ::pread(fd_, bytes.data() + at, bytes.size() - at,
                             std::uint64_t(address) - geometry_.base + at);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        return Status::io_error;
      }
      at += static_cast<std::size_t>(n);
    }
    return Status::ok;
  }

  bool FileFlash::Write(std::uint32_t offset,
                        std::span<const std::byte> bytes) {
    std::size_t at = 0;
    while (at < bytes.size()) {
      const auto n = ::pwrite(fd_, bytes.data() + at, bytes.size() - at,
                              std::uint64_t(offset) + at);
      if (n < 0 && errno == EINTR) {
        continue;
      }
      if (n <= 0) {
        return false;
      }
      at += static_cast<std::size_t>(n);
    }
    return true;
  }

  core::Status FileFlash::Erase(std::uint32_t address) {
    if (fd_ < 0) {
      return Status::not_running;
    }
    if (requested_ || cs != State::idle) {
      return Status::busy;
    }
    if (!Range(address, geometry_.sector_size) ||
        address % geometry_.sector_size) {
      return Status::invalid_argument;
    }
    address_ = address;
    operation_ = Operation::erase;
    requested_ = true;
    return Status::ok;
  }

  core::Status FileFlash::Program(std::uint32_t address,
                                  std::span<const std::byte> bytes) {
    if (fd_ < 0) {
      return Status::not_running;
    }
    if (requested_ || cs != State::idle) {
      return Status::busy;
    }
    if (bytes.size() != 16 || address % 16 || !Range(address, bytes.size())) {
      return Status::invalid_argument;
    }
    std::array<std::byte, 16> old;
    auto status = Read(address, old);
    if (status != Status::ok) {
      return status;
    }
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      if ((old[i] & bytes[i]) != bytes[i]) {
        return Status::invalid_argument;
      }
    }
    std::copy(bytes.begin(), bytes.end(), word_.begin());
    address_ = address;
    operation_ = Operation::program;
    requested_ = true;
    return Status::ok;
  }

  core::Status FileFlash::Poll() {
    State ns = cs;
    auto status = fd_ < 0 ? Status::not_running : Status::ok;
    switch (cs) {
      case State::idle:
        if (closed_) {
          closed_ = false;
        } else if (requested_) {
          ns = State::execute;
          status = Status::busy;
        }
        break;
      case State::execute:
        if (closed_) {
          closed_ = false;
        } else {
          bool written = true;
          if (operation_ == Operation::program) {
            written = Write(address_ - geometry_.base, word_);
          } else {
            std::array<std::byte, 4096> erased;
            erased.fill(std::byte{0xff});
            for (std::uint32_t at = 0; at < geometry_.sector_size && written;) {
              auto bytes = std::span(erased).first(std::min<std::size_t>(
                  erased.size(), geometry_.sector_size - at));
              written = Write(address_ - geometry_.base + at, bytes);
              at += bytes.size();
            }
          }
          if (!written || ::fsync(fd_) != 0) {
            status = Status::io_error;
          }
        }
        requested_ = false;
        ns = State::idle;
        break;
    }
    if (ns != cs) {
      cs = ns;
    }
    return status;
  }

  core::Time FileFlash::Now() const {
    if (clock_) {
      return clock_(clock_context_);
    }
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  boot::Flash FileFlash::driver() {
    return {
        this,
        [](void* p, std::uint32_t address, std::span<std::byte> bytes) {
          return static_cast<FileFlash*>(p)->Read(address, bytes);
        },
        [](void* p, std::uint32_t address) {
          return static_cast<FileFlash*>(p)->Erase(address);
        },
        [](void* p, std::uint32_t address, std::span<const std::byte> bytes) {
          return static_cast<FileFlash*>(p)->Program(address, bytes);
        },
        [](void* p) { return static_cast<FileFlash*>(p)->Poll(); },
        [](void* p) { return static_cast<FileFlash*>(p)->Now(); }};
  }


}  // namespace daveos::platform::host
