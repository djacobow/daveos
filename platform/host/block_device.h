#pragma once

#include "storage/block_device.h"

namespace daveos::platform::host {


  // Read-only regular-file media. Explicit open; no heap, mapping or global
  // instance. The file/device must outlive every attached Volume. All calls
  // use the same application thread. Open/close reject mounted-media leases.
  class FileBlockDevice {
   public:
    ~FileBlockDevice();
    FileBlockDevice() = default;
    FileBlockDevice(const FileBlockDevice&) = delete;
    FileBlockDevice& operator=(const FileBlockDevice&) = delete;
    bool open(const char* path);
    bool close();
    storage::BlockDevice device();

   private:
    bool Read(std::uint32_t sector, std::span<std::uint8_t> bytes);
    int descriptor_ = -1;  // POSIX descriptor, not a wire/storage integer.
    std::uint64_t sectors_ = 0;
    bool leased_ = false;
  };


}  // namespace daveos::platform::host
