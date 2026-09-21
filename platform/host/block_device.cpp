#include "block_device.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>

namespace daveos::platform::host {


  FileBlockDevice::~FileBlockDevice() {
    if (descriptor_ >= 0) {
      ::close(descriptor_);
    }
  }

  bool FileBlockDevice::open(const char* path) {
    if (!path || descriptor_ >= 0 || leased_) {
      return false;
    }
    auto fd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      return false;
    }

    struct stat info {};

    if (::fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        info.st_size % storage::kSectorBytes) {
      ::close(fd);
      return false;
    }
    descriptor_ = fd;
    sectors_ = static_cast<std::uint64_t>(info.st_size) / storage::kSectorBytes;
    return true;
  }

  bool FileBlockDevice::close() {
    if (leased_) {
      return false;
    }
    if (descriptor_ >= 0) {
      ::close(descriptor_);
      descriptor_ = -1;
    }
    sectors_ = 0;
    return true;
  }

  bool FileBlockDevice::Read(std::uint32_t sector,
                             std::span<std::uint8_t> bytes) {
    if (!leased_ || descriptor_ < 0 || bytes.empty() ||
        bytes.size() % storage::kSectorBytes ||
        std::uint64_t{sector} + bytes.size() / storage::kSectorBytes >
            sectors_) {
      return false;
    }
    const auto offset = std::uint64_t{sector} * storage::kSectorBytes;
    std::size_t done = 0;
    while (done < bytes.size()) {
      auto received =
          ::pread(descriptor_, bytes.data() + done, bytes.size() - done,
                  static_cast<off_t>(offset + done));
      if (received < 0 && errno == EINTR) {
        continue;
      }
      if (received <= 0) {
        return false;
      }
      done += static_cast<std::size_t>(received);
    }
    return true;
  }

  storage::BlockDevice FileBlockDevice::device() {
    return {this,
            [](void* p) {
              return static_cast<FileBlockDevice*>(p)->descriptor_ >= 0;
            },
            [](void* p) { return static_cast<FileBlockDevice*>(p)->sectors_; },
            [](void* p, std::uint32_t sector, std::span<std::uint8_t> bytes) {
              return static_cast<FileBlockDevice*>(p)->Read(sector, bytes);
            },
            [](void* p) {
              auto& self = *static_cast<FileBlockDevice*>(p);
              if (self.leased_ || self.descriptor_ < 0) {
                return false;
              }
              self.leased_ = true;
              return true;
            },
            [](void* p) { static_cast<FileBlockDevice*>(p)->leased_ = false; }};
  }


}  // namespace daveos::platform::host
