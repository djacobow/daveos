#include "reliability.h"

#include <cstring>

#include "stm32h563xx.h"
#include "util/wire.h"

namespace daveos::platform::stm32h5 {


  namespace {
    constexpr std::uint32_t kFlashBase = 0x08000000;
    constexpr std::uint32_t kFlashEnd = 0x08200000;
    constexpr std::uint32_t kCacheWaitBudget = 64000;
    constexpr std::uint32_t kWatchdogUpdateBudget = 1000000;
    constexpr std::uint32_t kEarlyWarningTicks = 64;
    constexpr std::uint32_t kWatchdogUpdateFlags =
        IWDG_SR_PVU | IWDG_SR_RVU | IWDG_SR_WVU | IWDG_SR_EWU;
    constexpr std::uint32_t kErrors =
        FLASH_SR_WRPERR | FLASH_SR_PGSERR | FLASH_SR_STRBERR | FLASH_SR_INCERR |
        FLASH_SR_OBKERR | FLASH_SR_OBKWERR | FLASH_SR_OPTCHANGEERR;
    constexpr std::uint32_t kBusy =
        FLASH_SR_BSY | FLASH_SR_WBNE | FLASH_SR_DBNE;
    // Interrupt bridge for the one CPU's currently executing validation read.
    volatile bool reading = false, read_error = false;
    util::Version fault_version;
    std::uint64_t fault_installation = 0;
    bool watchdog_failure_recorded = false;

    // Independent of SysTick so startup failures remain bounded.
    bool WatchdogSynchronized() {
      std::uint32_t budget = kWatchdogUpdateBudget;
      // ONF remains set while enabled; only synchronization flags clear.
      while ((IWDG->SR & kWatchdogUpdateFlags) && --budget) {
        __NOP();
      }
      return budget != 0;
    }

    bool Range(std::uint32_t address, std::size_t length) {
      return address >= kFlashBase && address < kFlashEnd &&
             length <= kFlashEnd - address;
    }
  }  // namespace

  core::Status Flash::Unlock() {
    if (pending_ || (FLASH->NSSR & kBusy)) {
      return core::Status::busy;
    }
    if (FLASH->NSCR & FLASH_CR_LOCK) {
      FLASH->NSKEYR = 0x45670123;
      FLASH->NSKEYR = 0xcdef89ab;
    }
    if (FLASH->NSCR & FLASH_CR_LOCK) {
      return core::Status::io_error;
    }
    FLASH->NSCCR = kErrors | FLASH_SR_EOP;
    return core::Status::ok;
  }

  core::Status Flash::Read(std::uint32_t address, std::span<std::byte> bytes) {
    if (!Range(address, bytes.size())) {
      return core::Status::invalid_argument;
    }
    read_error = false;
    reading = true;
    __DSB();
    const auto* source =
        reinterpret_cast<const volatile std::uint8_t*>(address);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
      bytes[i] = std::byte(source[i]);
    }
    __DSB();
    reading = false;
    return read_error ? core::Status::io_error : core::Status::ok;
  }

  core::Status Flash::Erase(std::uint32_t address) {
    if (!Range(address, 8192) || address % 8192) {
      return core::Status::invalid_argument;
    }
    auto status = Unlock();
    if (status != core::Status::ok) {
      return status;
    }
    auto sector = ((address - kFlashBase) % 0x100000) / 8192;
    auto bank = address >= 0x08100000 ? FLASH_CR_BKSEL : 0u;
    FLASH->NSCR = bank | FLASH_CR_SER | (sector << FLASH_CR_SNB_Pos);
    pending_ = true;
    FLASH->NSCR = FLASH->NSCR | FLASH_CR_START;
    return core::Status::ok;
  }

  core::Status Flash::Program(std::uint32_t address,
                              std::span<const std::byte> bytes) {
    if (!Range(address, 16) || address % 16 || bytes.size() != 16) {
      return core::Status::invalid_argument;
    }
    auto status = Unlock();
    if (status != core::Status::ok) {
      return status;
    }
    FLASH->NSCR = FLASH_CR_PG;
    auto mask = __get_PRIMASK();
    __disable_irq();
    auto* output = reinterpret_cast<volatile std::uint32_t*>(address);
    for (std::size_t i = 0; i < 4; ++i) {
      output[i] = util::wire::read32(bytes, i * 4);
    }
    __DSB();
    pending_ = true;
    __set_PRIMASK(mask);
    return core::Status::ok;
  }

  core::Status Flash::Poll() {
    if ((FLASH->NSSR & kBusy) || (ICACHE->SR & ICACHE_SR_BUSYF)) {
      return core::Status::busy;
    }
    if (!pending_) {
      return core::Status::ok;
    }
    auto errors = FLASH->NSSR & kErrors;
    FLASH->NSCR = FLASH_CR_LOCK;
    FLASH->NSCCR = kErrors | FLASH_SR_EOP;
    pending_ = false;
    __DSB();
    // ICACHE also services flash data reads on H563. A journal scan can cache
    // erased words, so invalidate after every completed write/erase before
    // exposing completion to readback callers, including partially failed
    // writes.
    if (ICACHE->CR & ICACHE_CR_EN) {
      ICACHE->CR = ICACHE->CR | ICACHE_CR_CACHEINV;
      std::uint32_t budget = kCacheWaitBudget;
      while ((ICACHE->SR & ICACHE_SR_BUSYF) && --budget) {
        __NOP();
      }
      if (!budget) {
        return core::Status::timeout;
      }
      ICACHE->FCR = ICACHE_FCR_CBSYENDF;
      __DSB();
      __ISB();
    }
    return errors ? core::Status::io_error : core::Status::ok;
  }

  boot::Flash Flash::driver() {
    return {
        this,
        [](void* p, std::uint32_t address, std::span<std::byte> bytes) {
          return static_cast<Flash*>(p)->Read(address, bytes);
        },
        [](void* p, std::uint32_t address) {
          return static_cast<Flash*>(p)->Erase(address);
        },
        [](void* p, std::uint32_t address, std::span<const std::byte> bytes) {
          return static_cast<Flash*>(p)->Program(address, bytes);
        },
        [](void* p) { return static_cast<Flash*>(p)->Poll(); },
        [](void* p) {
          auto& self = *static_cast<Flash*>(p);
          return self.clock_(self.clock_context_);
        }};
  }

  core::Status Watchdog::Start(core::Time timeout) {
    if (started_) {
      return core::Status::already_initialized;
    }
    // Nominal LSI frequency. Timeout includes oscillator tolerance, as with
    // HAL.
    std::uint32_t prescaler = 0;
    std::uint64_t ticks = 0;
    for (; prescaler <= 8; ++prescaler) {
      auto divisor = UINT64_C(4) << prescaler;
      if (timeout > core::kForever / 32000) {
        return core::Status::invalid_argument;
      }
      const auto numerator = timeout * 32000;
      const auto denominator = divisor * 1000000;
      ticks = numerator / denominator + (numerator % denominator != 0);
      if (ticks && ticks <= 4096) {
        break;
      }
    }
    if (!ticks || ticks > 4096) {
      return core::Status::invalid_argument;
    }
    DBGMCU->APB1FZR1 = DBGMCU->APB1FZR1 | DBGMCU_APB1FZR1_DBG_IWDG_STOP;
    IWDG->KR = 0xcccc;
    started_ = true;
    IWDG->KR = 0x5555;
    IWDG->PR = prescaler;
    IWDG->RLR = static_cast<std::uint32_t>(ticks - 1);
    // Reload must be synchronized before programming the early comparator.
    if (!WatchdogSynchronized()) {
      return core::Status::timeout;
    }
    IWDG->EWCR = IWDG_EWCR_EWIC |
                 (ticks > 1 ? IWDG_EWCR_EWIE | static_cast<std::uint32_t>(
                                                   ticks > kEarlyWarningTicks
                                                       ? kEarlyWarningTicks
                                                       : ticks - 1)
                            : 0u);
    if (!WatchdogSynchronized()) {
      return core::Status::timeout;
    }
    IWDG->KR = 0xaaaa;
    NVIC_ClearPendingIRQ(IWDG_IRQn);
    NVIC_SetPriority(IWDG_IRQn, 0);
    NVIC_EnableIRQ(IWDG_IRQn);
    return core::Status::ok;
  }

  core::Status Watchdog::Feed() {
    if (!started_) {
      return core::Status::not_running;
    }
    IWDG->KR = 0xaaaa;
    return core::Status::ok;
  }

  watchdog::Driver Watchdog::driver() {
    return {this,
            [](void* p, core::Time timeout) {
              return static_cast<Watchdog*>(p)->Start(timeout);
            },
            [](void* p) { return static_cast<Watchdog*>(p)->Feed(); }};
  }

  util::fault::Record& retained_fault() {
    // Linker reserves the first 2 KiB of SRAM for this 256-byte record and an
    // emergency exception stack. Neither bootloader nor application clears it.
    return *reinterpret_cast<util::fault::Record*>(0x20000000);
  }

  bool handle_flash_ecc() {
    if (reading && (FLASH->ECCDETR & FLASH_ECCR_ECCD)) {
      read_error = true;
      FLASH->ECCDETR = FLASH->ECCDETR | FLASH_ECCR_ECCD;
      __DSB();
      return true;
    }
    return false;
  }

  void set_fault_identity(const util::Version& version,
                          std::uint64_t installation) {
    fault_version = version;
    fault_installation = installation;
  }

  void record_failure(util::fault::Data data) {
    data.version = fault_version;
    data.installation = fault_installation;
    util::fault::save(retained_fault(), data);
    watchdog_failure_recorded = data.kind == util::fault::Kind::watchdog;
    __DSB();
  }

  [[noreturn]] void reset() { NVIC_SystemReset(); }


}  // namespace daveos::platform::stm32h5

extern "C" void DaveOS_FaultCapture(const std::uint32_t* frame,
                                    std::uint32_t exc_return,
                                    std::uint32_t kind) {
  namespace h5 = daveos::platform::stm32h5;
  namespace fault = daveos::util::fault;
  fault::Data data;
  // A cooperative health check may already have recorded the precise failing
  // task. Enrich that current-boot record with the interrupted frame.
  if (kind == static_cast<std::uint32_t>(fault::Kind::watchdog)) {
    if (h5::watchdog_failure_recorded) {
      fault::read(h5::retained_fault(), data);
    }
    if (!data.check[0]) {
      fault::copy_name(data.check, "iwdg_early_warning");
      data.status = static_cast<std::uint32_t>(daveos::core::Status::timeout);
    }
    IWDG->EWCR = IWDG->EWCR | IWDG_EWCR_EWIC;
    NVIC_DisableIRQ(IWDG_IRQn);
  }
  if (kind <= static_cast<std::uint32_t>(fault::Kind::usage_fault)) {
    constexpr const char* kFaultNames[] = {"hard_fault", "memory_fault",
                                           "bus_fault", "usage_fault"};
    fault::copy_name(data.check, kFaultNames[kind]);
    fault::copy_name(data.module, "core");
    fault::copy_name(data.task, "exception");
    data.status =
        static_cast<std::uint32_t>(daveos::core::Status::health_failed);
  }
  data.kind = static_cast<fault::Kind>(kind);
  data.version = h5::fault_version;
  data.installation = h5::fault_installation;
  data.exc_return = exc_return;
  data.stack = reinterpret_cast<std::uintptr_t>(frame);
  data.cfsr = SCB->CFSR;
  data.hfsr = SCB->HFSR;
  data.mmfar = SCB->MMFAR;
  data.bfar = SCB->BFAR;
  // The basic registers are first; extended floating-point state follows them.
  // Stacking errors make even an in-RAM frame unreliable.
  auto address = reinterpret_cast<std::uintptr_t>(frame);
  constexpr std::uint32_t kStackErrors = (1u << 3) | (1u << 4) | (1u << 5) |
                                         (1u << 11) | (1u << 12) | (1u << 13) |
                                         SCB_CFSR_STKOF_Msk;
  if (!(data.cfsr & kStackErrors) && address >= 0x20000800 &&
      address <= 0x200a0000 - 32 && !(address & 3)) {
    const auto* source =
        reinterpret_cast<const volatile std::uint32_t*>(address);
    for (std::size_t i = 0; i < data.frame.size(); ++i) {
      data.frame[i] = source[i];
    }
    data.frame_valid = true;
  }
  fault::save(h5::retained_fault(), data);
  __DSB();
  if (DCB->DHCSR & DCB_DHCSR_C_DEBUGEN_Msk) {
    __BKPT(0);
  }
  if (kind == static_cast<std::uint32_t>(fault::Kind::watchdog)) {
    // Never return to code that might feed again. Hardware supplies the reset.
    while (true) {
      __WFI();
    }
  }
  h5::reset();
}
