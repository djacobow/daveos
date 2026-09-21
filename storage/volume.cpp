#include "volume.h"

#include <algorithm>
#include <limits>

#include "diskio.h"

namespace {
  // FatFs's C ABI has no context parameter. This is the only global bridge;
  // neither media nor filesystem instances are global singletons. Registration
  // is explicit and released by the owning Volume. Access is single-threaded.
  std::array<const daveos::storage::BlockDevice*, FF_VOLUMES> drives{};

  const daveos::storage::BlockDevice* Device(BYTE index) {
    return index < drives.size() ? drives[index] : nullptr;
  }
}  // namespace

extern "C" DSTATUS disk_status(BYTE index) {
  const auto* device = Device(index);
  return device && device->ready(device->context) ? 0 : STA_NOINIT;
}

extern "C" DSTATUS disk_initialize(BYTE index) { return disk_status(index); }

extern "C" DRESULT disk_read(BYTE index, BYTE* destination, LBA_t sector,
                             UINT count) {
  const auto* device = Device(index);
  if (!device || !device->ready(device->context)) {
    return RES_NOTRDY;
  }
  if (!destination || !count ||
      std::uint64_t{sector} + count > device->sectors(device->context) ||
      std::uint64_t{count} * daveos::storage::kSectorBytes >
          std::numeric_limits<std::size_t>::max()) {
    return RES_PARERR;
  }
  return device->read(
             device->context, sector,
             {destination, std::size_t{count} * daveos::storage::kSectorBytes})
             ? RES_OK
             : RES_ERROR;
}

extern "C" DRESULT disk_ioctl(BYTE index, BYTE command, void* output) {
  const auto* device = Device(index);
  if (!device || !device->ready(device->context)) {
    return RES_NOTRDY;
  }
  if (command == CTRL_SYNC) {
    return RES_OK;
  }
  if (!output) {
    return RES_PARERR;
  }
  if (command == GET_SECTOR_SIZE) {
    *static_cast<WORD*>(output) = daveos::storage::kSectorBytes;
    return RES_OK;
  }
  if (command == GET_SECTOR_COUNT &&
      device->sectors(device->context) <= std::numeric_limits<LBA_t>::max()) {
    *static_cast<LBA_t*>(output) =
        static_cast<LBA_t>(device->sectors(device->context));
    return RES_OK;
  }
  return RES_PARERR;
}

namespace daveos::storage {


  Volume::~Volume() {
    (void)unmount();
    if (attached_) {
      drives[static_cast<std::size_t>(drive_[0] - '0')] = nullptr;
    }
  }

  FRESULT Volume::attach() {
    Guard guard(*this);
    if (!guard || attached_) {
      return FR_LOCKED;
    }
    if (!device_.ready || !device_.sectors || !device_.read ||
        !device_.acquire || !device_.release) {
      return FR_INVALID_PARAMETER;
    }
    for (std::size_t i = 0; i < drives.size(); ++i) {
      if (!drives[i]) {
        drives[i] = &device_;
        drive_ = {static_cast<char>('0' + i), ':', '\0'};
        attached_ = true;
        return FR_OK;
      }
    }
    return FR_TOO_MANY_OPEN_FILES;
  }

  FRESULT Volume::mount() {
    Guard guard(*this);
    if (!guard || file_open_ || directory_open_) {
      return FR_LOCKED;
    }
    if (!attached_ || !device_.ready(device_.context)) {
      return FR_NOT_READY;
    }
    if (mounted_) {
      return FR_OK;
    }
    if (!device_.acquire(device_.context)) {
      return FR_LOCKED;
    }
    auto result = f_mount(&fs_, drive_.data(), 1);
    if (result == FR_OK) {
      mounted_ = true;
    } else {
      (void)f_mount(nullptr, drive_.data(), 0);
      device_.release(device_.context);
    }
    return result;
  }

  FRESULT Volume::unmount() {
    Guard guard(*this);
    if (!guard) {
      return FR_LOCKED;
    }
    auto result = Close();
    if (mounted_) {
      (void)f_mount(nullptr, drive_.data(), 0);
      mounted_ = false;
      device_.release(device_.context);
    }
    return result;
  }

  FRESULT Volume::Path(std::string_view path,
                       std::array<char, kPathCapacity + 4>& output) const {
    if (!mounted_) {
      return FR_NOT_READY;
    }
    if (path.size() > kPathCapacity || path.find(':') != path.npos ||
        path.find('\0') != path.npos) {
      return FR_INVALID_NAME;
    }
    output[0] = drive_[0];
    output[1] = ':';
    output[2] = '/';
    std::copy(path.begin(), path.end(), output.begin() + 3);
    output[path.size() + 3] = '\0';
    return FR_OK;
  }

  FRESULT Volume::open(std::string_view path, std::uint32_t offset) {
    Guard guard(*this);
    if (!guard || file_open_ || directory_open_) {
      return FR_LOCKED;
    }
    std::array<char, kPathCapacity + 4> name{};
    auto result = Path(path, name);
    if (result != FR_OK) {
      return result;
    }
    result = f_open(&file_, name.data(), FA_READ);
    if (result != FR_OK) {
      return result;
    }
    file_open_ = true;
    if (offset > f_size(&file_)) {
      (void)Close();
      return FR_INVALID_PARAMETER;
    }
    result = f_lseek(&file_, offset);
    if (result != FR_OK) {
      (void)Close();
    }
    return result;
  }

  FRESULT Volume::read(std::span<std::uint8_t> bytes, std::uint32_t& count) {
    Guard guard(*this);
    count = 0;
    if (!guard) {
      return FR_LOCKED;
    }
    if (!file_open_) {
      return FR_INVALID_OBJECT;
    }
    if (bytes.size() > std::numeric_limits<UINT>::max()) {
      return FR_INVALID_PARAMETER;
    }
    UINT received = 0;
    auto result = f_read(&file_, bytes.data(), static_cast<UINT>(bytes.size()),
                         &received);
    count = received;
    return result;
  }

  FRESULT Volume::open_directory(std::string_view path) {
    Guard guard(*this);
    if (!guard || file_open_ || directory_open_) {
      return FR_LOCKED;
    }
    std::array<char, kPathCapacity + 4> name{};
    auto result = Path(path, name);
    if (result != FR_OK) {
      return result;
    }
    result = f_opendir(&directory_, name.data());
    directory_open_ = result == FR_OK;
    return result;
  }

  FRESULT Volume::next(FILINFO& entry) {
    Guard guard(*this);
    if (!guard) {
      return FR_LOCKED;
    }
    return directory_open_ ? f_readdir(&directory_, &entry) : FR_INVALID_OBJECT;
  }

  FRESULT Volume::Close() {
    auto result = FR_OK;
    if (file_open_) {
      result = f_close(&file_);
      file_open_ = false;
    }
    if (directory_open_) {
      result = f_closedir(&directory_);
      directory_open_ = false;
    }
    return result;
  }

  FRESULT Volume::close() {
    Guard guard(*this);
    return guard ? Close() : FR_LOCKED;
  }

  const char* result_name(FRESULT result) {
    constexpr std::array names{"ok",
                               "disk_error",
                               "internal_error",
                               "not_ready",
                               "no_file",
                               "no_path",
                               "invalid_name",
                               "denied",
                               "exists",
                               "invalid_object",
                               "write_protected",
                               "invalid_drive",
                               "not_enabled",
                               "no_filesystem",
                               "mkfs_aborted",
                               "timeout",
                               "locked",
                               "no_memory",
                               "too_many_open_files",
                               "invalid_parameter"};
    const auto index = static_cast<std::size_t>(result);
    return index < names.size() ? names[index] : "unknown";
  }


}  // namespace daveos::storage
