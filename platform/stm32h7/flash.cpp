#include "flash.h"

#include <algorithm>

#include "stm32h755xx.h"
#include "util/wire.h"

namespace daveos::platform::stm32h7 {


  namespace {
    constexpr std::uint32_t kBase = 0x08000000;
    constexpr std::uint32_t kBankSize = 1024 * 1024;
    constexpr std::uint32_t kSectorSize = 128 * 1024;
    constexpr std::uint32_t kWordSize = 32;
    constexpr std::uint32_t kBusy = FLASH_SR_BSY | FLASH_SR_QW | FLASH_SR_WBNE;
    constexpr std::uint32_t kErrors =
        FLASH_SR_WRPERR | FLASH_SR_PGSERR | FLASH_SR_STRBERR | FLASH_SR_INCERR |
        FLASH_SR_OPERR | FLASH_SR_RDPERR | FLASH_SR_RDSERR | FLASH_SR_DBECCERR;
    Flash* owner = nullptr;

    volatile std::uint32_t& Control(std::uint32_t bank) {
      return bank ? FLASH->CR2 : FLASH->CR1;
    }

    volatile std::uint32_t& Status(std::uint32_t bank) {
      return bank ? FLASH->SR2 : FLASH->SR1;
    }

    volatile std::uint32_t& Clear(std::uint32_t bank) {
      return bank ? FLASH->CCR2 : FLASH->CCR1;
    }

    bool Range(std::uint32_t address, std::size_t size) {
      return address >= kBase && address < kBase + 2 * kBankSize &&
             size <= kBase + 2 * kBankSize - address;
    }
  }  // namespace

  std::uint32_t Flash::ExecutingBank() {
    const auto vector = SCB->VTOR;
    for (std::uint32_t bank = 0; bank < 2; ++bank) {
      const auto base = kBase + bank * kBankSize;
      if (vector >= base + 2 * kSectorSize && vector < base + kBankSize) {
        return bank;
      }
    }
    return 2;
  }

  core::Status Flash::Unlock(std::uint32_t bank) {
    if (owner || pending_ || (Status(bank) & kBusy)) {
      return core::Status::busy;
    }
    if (Control(bank) & FLASH_CR_LOCK) {
      auto& key = bank ? FLASH->KEYR2 : FLASH->KEYR1;
      key = 0x45670123;
      key = 0xcdef89ab;
    }
    if (Control(bank) & FLASH_CR_LOCK) {
      return core::Status::io_error;
    }
    Clear(bank) = kErrors | FLASH_SR_SNECCERR | FLASH_SR_EOP;
    bank_ = bank;
    owner = this;
    return core::Status::ok;
  }

  core::Status Flash::Read(std::uint32_t address, std::span<std::byte> bytes) {
    if (!Range(address, bytes.size())) {
      return core::Status::invalid_argument;
    }
    if (owner || ((FLASH->SR1 | FLASH->SR2) & kBusy)) {
      return core::Status::busy;
    }
    bool error = false;
    // Guard only explicit flash validation loads. BFHFNMIGN with FAULTMASK
    // permits inspecting DBECCERR instead of escalating an ECC data BusFault.
    // Restore masks between flash words so a scan cannot starve interrupts.
    for (std::size_t at = 0; at < bytes.size();) {
      const auto bank = (address + at - kBase) / kBankSize;
      const auto count = std::min<std::size_t>(
          kWordSize - (address + at) % kWordSize, bytes.size() - at);
      const auto mask = __get_FAULTMASK();
      const auto ccr = SCB->CCR;
      __disable_fault_irq();
      SCB->CCR = ccr | SCB_CCR_BFHFNMIGN_Msk;
      Clear(bank) = FLASH_SR_DBECCERR | FLASH_SR_SNECCERR;
      __DSB();
      __ISB();
      const auto* source =
          reinterpret_cast<const volatile std::uint8_t*>(address + at);
      for (std::size_t i = 0; i < count; ++i) {
        bytes[at + i] = std::byte(source[i]);
      }
      __DSB();
      error = error || (Status(bank) & FLASH_SR_DBECCERR);
      Clear(bank) = FLASH_SR_DBECCERR | FLASH_SR_SNECCERR;
      SCB->CCR = ccr;
      __DSB();
      __ISB();
      __set_FAULTMASK(mask);
      at += count;
    }
    if (progress_ && !error) {
      progress_(context_);
    }
    return error ? core::Status::io_error : core::Status::ok;
  }

  core::Status Flash::Erase(std::uint32_t address) {
    if (!Range(address, kSectorSize) || address % kSectorSize) {
      return core::Status::invalid_argument;
    }
    const auto bank = (address - kBase) / kBankSize;
    const auto sector = (address - kBase) % kBankSize / kSectorSize;
    if (!sector || bank == ExecutingBank()) {
      return core::Status::unsupported;
    }
    auto result = Unlock(bank);
    if (result != core::Status::ok) {
      return result;
    }
    address_ = address;
    size_ = kSectorSize;
    pending_ = true;
    Control(bank) =
        FLASH_CR_PSIZE_1 | FLASH_CR_SER | (sector << FLASH_CR_SNB_Pos);
    Control(bank) = Control(bank) | FLASH_CR_START;
    return core::Status::ok;
  }

  core::Status Flash::Program(std::uint32_t address,
                              std::span<const std::byte> bytes) {
    if (!Range(address, kWordSize) || address % kWordSize ||
        bytes.size() != kWordSize) {
      return core::Status::invalid_argument;
    }
    const auto bank = (address - kBase) / kBankSize;
    const auto offset = (address - kBase) % kBankSize;
    if (offset < kSectorSize ||
        (bank == ExecutingBank() && offset >= 2 * kSectorSize)) {
      return core::Status::unsupported;
    }
    // Copy before setting PG: source may itself reside in the bank to program.
    std::array<std::uint32_t, 8> data;
    for (std::size_t i = 0; i < data.size(); ++i) {
      data[i] = util::wire::read32(bytes, i * 4);
    }
    auto result = Unlock(bank);
    if (result != core::Status::ok) {
      return result;
    }
    address_ = address;
    size_ = kWordSize;
    pending_ = true;
    const auto mask = __get_PRIMASK();
    __disable_irq();
    Control(bank) = FLASH_CR_PSIZE_1 | FLASH_CR_PG;
    __DSB();
    __ISB();
    auto* target = reinterpret_cast<volatile std::uint32_t*>(address);
    for (std::size_t i = 0; i < data.size(); ++i) {
      target[i] = data[i];
    }
    __DSB();
    __set_PRIMASK(mask);
    return core::Status::ok;
  }

  core::Status Flash::Poll() {
    if (owner && owner != this) {
      return core::Status::busy;
    }
    if (!pending_) {
      return core::Status::ok;
    }
    if (Status(bank_) & kBusy) {
      return core::Status::busy;
    }
    const auto errors = Status(bank_) & kErrors;
    Control(bank_) = FLASH_CR_LOCK;
    Clear(bank_) = kErrors | FLASH_SR_SNECCERR | FLASH_SR_EOP;
    __DSB();
    if (SCB->CCR & SCB_CCR_DC_Msk) {
      SCB_InvalidateDCache_by_Addr(reinterpret_cast<std::uint32_t*>(address_),
                                   size_);
    }
    if (SCB->CCR & SCB_CCR_IC_Msk) {
      SCB_InvalidateICache();
    }
    __DSB();
    __ISB();
    pending_ = false;
    owner = nullptr;
    if (progress_ && !errors) {
      progress_(context_);
    }
    return errors ? core::Status::io_error : core::Status::ok;
  }

  boot::Flash Flash::driver() {
    return {this,
            [](void* p, std::uint32_t a, std::span<std::byte> b) {
              return static_cast<Flash*>(p)->Read(a, b);
            },
            [](void* p, std::uint32_t a) {
              return static_cast<Flash*>(p)->Erase(a);
            },
            [](void* p, std::uint32_t a, std::span<const std::byte> b) {
              return static_cast<Flash*>(p)->Program(a, b);
            },
            [](void* p) { return static_cast<Flash*>(p)->Poll(); },
            [](void* p) {
              auto& s = *static_cast<Flash*>(p);
              return s.clock_(s.context_);
            },
            [](void*) { return ExecutingBank(); }};
  }


}  // namespace daveos::platform::stm32h7
