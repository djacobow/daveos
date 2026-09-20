#include "otp.h"

#include "flash_read.h"
#include "stm32h563xx.h"

namespace daveos::platform::stm32h5 {


  namespace {
    using Status = core::Status;
    constexpr core::Time kProgramTimeout = 100000, kLockTimeout = 1000000;
    constexpr std::uint32_t kWaitBudget = 6400000;
    constexpr std::uint32_t kNonCacheable = 0x44;
  }  // namespace

  bool Otp::Mapping() const {
    if (!(MPU->CTRL & MPU_CTRL_ENABLE_Msk) ||
        mpu_region_ >= ((MPU->TYPE >> 8) & 0xff)) {
      return false;
    }
    const auto mask = __get_PRIMASK();
    __disable_irq();
    const auto selected = MPU->RNR;
    MPU->RNR = mpu_region_;
    const auto base = MPU->RBAR, limit = MPU->RLAR;
    const auto attr = (limit & MPU_RLAR_AttrIndx_Msk) >> MPU_RLAR_AttrIndx_Pos;
    const auto mair = attr < 4 ? MPU->MAIR0 : MPU->MAIR1;
    MPU->RNR = selected;
    __set_PRIMASK(mask);
    return (limit & MPU_RLAR_EN_Msk) && (base & MPU_RBAR_XN_Msk) &&
           (base & MPU_RBAR_BASE_Msk) <= detail::kOtpBase &&
           ((limit & MPU_RLAR_LIMIT_Msk) | 31) >=
               detail::kOtpBase + detail::kOtpSize - 1 &&
           ((mair >> ((attr % 4) * 8)) & 0xff) == kNonCacheable;
  }

  std::uint32_t Otp::Permissions(std::uint32_t access) {
    const auto mask = __get_PRIMASK();
    __disable_irq();
    __DMB();
    const auto selected = MPU->RNR;
    MPU->RNR = mpu_region_;
    const auto original = MPU->RBAR;
    MPU->RBAR = (original & ~MPU_RBAR_AP_Msk) | access;
    MPU->RNR = selected;
    __DSB();
    __ISB();
    __set_PRIMASK(mask);
    return original & MPU_RBAR_AP_Msk;
  }

  Status Otp::Init() {
    if (initialized_) {
      return Status::already_initialized;
    }
    if (!Mapping() || !flash_.clock_) {
      return Status::incompatible;
    }
    if (detail::flash_busy()) {
      return Status::busy;
    }
    // Refuse externally masked OTP NMIs: all-ones data alone proves nothing.
    if (SBS->ECCNMIR & SBS_ECCNMIR_ECCNMI_MASK_EN) {
      return Status::incompatible;
    }
    initialized_ = true;
    return Status::ok;
  }

  Status Otp::Inspect(std::uint32_t block, otp::Block& out) {
    if (!initialized_) {
      return Status::not_running;
    }
    if (block >= otp::kBlockCount) {
      return Status::invalid_argument;
    }
    if (timed_out_) {
      return Status::timeout;
    }
    if (detail::flash_busy()) {
      return Status::busy;
    }
    out = {};
    out.locked = FLASH->OTPBLR_CUR & (std::uint32_t{1} << block);
    Audit audit{};
    audit.locked = out.locked;
    for (std::uint32_t word = 0; word < 32; ++word) {
      const auto cell = detail::read_otp(detail::kOtpBase +
                                         block * otp::kRecordSize + word * 2);
      out.record.bytes[2 * word] = std::byte(cell.value & 0xff);
      out.record.bytes[2 * word + 1] = std::byte(cell.value >> 8);
      audit.virgin += cell.state == detail::Cell::virgin;
      audit.programmed += cell.state == detail::Cell::programmed;
      audit.unreadable += cell.state == detail::Cell::unreadable;
    }
    out.storage = audit.unreadable ? otp::Storage::unreadable
                  : (audit.programmed || out.locked || attempted_[block])
                      ? otp::Storage::consumed
                      : otp::Storage::unused;
    audit.fingerprint = util::crc32::calculate(out.record.bytes);
    audit_[block] = audit;
    return Status::ok;
  }

  Status Otp::Wait() {
    const auto start = flash_.clock_(flash_.clock_context_);
    const auto timeout =
        (FLASH->OPTCR & FLASH_OPTCR_OPTSTART) ? kLockTimeout : kProgramTimeout;
    for (std::uint32_t i = 0; i < kWaitBudget; ++i) {
      const auto status = flash_.Poll();
      if (status != Status::busy) {
        return status;
      }
      if (flash_.clock_(flash_.clock_context_) - start >= timeout) {
        break;
      }
    }
    timed_out_ = true;  // Retain shared controller ownership until reset.
    return Status::timeout;
  }

  otp::ProgramResult Otp::Program(std::uint32_t block,
                                  const otp::Record& record) {
    if (!allow_provisioning_) {
      return {Status::unsupported, false};
    }
    otp::Block before;
    auto status = Inspect(block, before);
    if (status != Status::ok) {
      return {status, false};
    }
    if (before.storage != otp::Storage::unused || before.locked) {
      return {Status::rejected, false};
    }
    if (!Mapping()) {
      return {Status::incompatible, false};
    }
    // All-RW during this bounded operation; preserve the board's other MPU
    // regions, attributes and engineering-byte mapping. Restore on every exit.
    const auto permissions = Permissions(1u << MPU_RBAR_AP_Pos);
    bool attempted = false;
    for (std::uint32_t step = 0; step < 32; ++step) {
      status = flash_.Unlock();
      if (status != Status::ok) {
        break;
      }
      const auto word = (step + 1) % 32;  // Length first, type last.
      const auto value = std::uint16_t(
          std::to_integer<std::uint16_t>(record.bytes[2 * word]) |
          (std::to_integer<std::uint16_t>(record.bytes[2 * word + 1]) << 8));
      attempted = attempted_[block] = true;
      FLASH->NSCR = FLASH_CR_PG;
      flash_.pending_ = true;
      *reinterpret_cast<volatile std::uint16_t*>(
          detail::kOtpBase + block * otp::kRecordSize + word * 2) = value;
      __DSB();
      status = Wait();
      if (status != Status::ok) {
        break;
      }
    }
    Permissions(permissions);
    return {status, attempted};
  }

  Status Otp::Lock(std::uint32_t block) {
    if (!allow_provisioning_) {
      return Status::unsupported;
    }
    otp::Block before;
    auto status = Inspect(block, before);
    if (status != Status::ok || before.locked) {
      return status;
    }
    if (before.storage != otp::Storage::consumed || !before.record.valid()) {
      return Status::rejected;
    }
    if (FLASH->OTPBLR_PRG != FLASH->OTPBLR_CUR) {
      return Status::busy;  // Do not launch someone else's staged OTP changes.
    }
    status = flash_.Unlock();
    if (status != Status::ok) {
      return status;
    }
    if (FLASH->OPTCR & FLASH_OPTCR_OPTLOCK) {
      FLASH->OPTKEYR = 0x08192a3b;
      FLASH->OPTKEYR = 0x4c5d6e7f;
    }
    if (FLASH->OPTCR & FLASH_OPTCR_OPTLOCK) {
      flash_.pending_ = true;
      status = Wait();  // Release our ownership and relock NSCR.
      return status == Status::ok ? Status::io_error : status;
    }
    const auto wanted = FLASH->OTPBLR_CUR | (std::uint32_t{1} << block);
    FLASH->OTPBLR_PRG = wanted;
    flash_.pending_ = true;
    FLASH->OPTCR = FLASH->OPTCR | FLASH_OPTCR_OPTSTART;
    status = Wait();
    if (status != Status::timeout) {
      FLASH->OPTCR = FLASH->OPTCR | FLASH_OPTCR_OPTLOCK;
    }
    if (status == Status::ok && FLASH->OTPBLR_CUR != wanted) {
      return Status::io_error;  // No implicit reset to load or retry options.
    }
    return status;
  }

  otp::Driver Otp::driver() {
    return {
        this,
        allow_provisioning_ ? "h563-otp" : "h563-otp-readonly",
        otp::kBlockCount,
        otp::kRecordSize,
        [](void* p) { return static_cast<Otp*>(p)->Init(); },
        [](void* p, std::uint32_t i, otp::Block& out) {
          return static_cast<Otp*>(p)->Inspect(i, out);
        },
        [](void* p, std::uint32_t i, const otp::Record& r) {
          return static_cast<Otp*>(p)->Program(i, r);
        },
        [](void* p, std::uint32_t i) { return static_cast<Otp*>(p)->Lock(i); }};
  }


}  // namespace daveos::platform::stm32h5
