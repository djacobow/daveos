#pragma once

#include <array>
#include <string_view>

#include "block_device.h"
#include "config.h"
#include "ff.h"

namespace daveos::storage {


  // Application-owned, nonmoving volume. Explicit attach() reserves
  // a numbered FatFs drive; construction does nothing. The injected device
  // must outlive this object. Destroy only after operations have unwound.
  // One file OR directory may be open. Every public operation rejects nested
  // access to this volume with FR_LOCKED; it never waits for an ancestor.
  // All volumes use one execution thread; host concurrency is not supported.
  class Volume {
   public:
    static constexpr std::size_t kPathCapacity = 255;

    explicit Volume(const BlockDevice& device) : device_(device) {}

    ~Volume();
    Volume(const Volume&) = delete;
    Volume& operator=(const Volume&) = delete;

    FRESULT attach();
    // Mode cannot change while mounted: unmount before opting into writes.
    FRESULT mount(bool writable = false);
    FRESULT create(std::string_view path);  // New files only; never truncate.
    FRESULT write(std::span<const std::uint8_t> bytes, std::uint32_t& count);
    FRESULT sync();
    // Read-write mounts only. Reject directories and open handles; sync
    // metadata.
    FRESULT remove(std::string_view path);
    FRESULT unmount();
    FRESULT open(std::string_view path, std::uint32_t offset = 0);
    FRESULT read(std::span<std::uint8_t> bytes, std::uint32_t& count);
    FRESULT open_directory(std::string_view path);
    FRESULT next(FILINFO& entry);  // Empty fname marks end, not an error.
    FRESULT close();

    bool mounted() const { return mounted_; }

   private:
    class Guard {
     public:
      explicit Guard(Volume& volume)
          : volume_(volume), entered_(!volume.busy_) {
        if (entered_) {
          volume_.busy_ = true;
        }
      }

      ~Guard() {
        if (entered_) {
          volume_.busy_ = false;
        }
      }

      explicit operator bool() const { return entered_; }

      Guard(const Guard&) = delete;
      Guard& operator=(const Guard&) = delete;

     private:
      Volume& volume_;
      bool entered_;
    };

    FRESULT Path(std::string_view path,
                 std::array<char, kPathCapacity + 4>& output) const;
    FRESULT Close();
    BlockDevice device_;
    FATFS fs_{};
    FIL file_{};
    DIR directory_{};
    std::array<char, 3> drive_{};
    bool attached_ = false, mounted_ = false, busy_ = false;
    bool file_open_ = false, directory_open_ = false, writable_ = false;
  };

  const char* result_name(FRESULT result);


}  // namespace daveos::storage
