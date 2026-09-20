#include "reliability.h"

#include "stm32h755xx.h"
#include "util/wire.h"

namespace daveos::platform::stm32h7 {


  namespace {
    constexpr std::uint32_t kWatchdogUpdateBudget = 1000000;
    constexpr std::uint32_t kFailureUartBudget = 64000;
    util::Version fault_version;
    std::uint64_t fault_installation = 0;
  }  // namespace

  core::Status Watchdog::Start(core::Time timeout) {
    if (started_) {
      return core::Status::already_initialized;
    }
    // Nominal LSI frequency. Timeout includes oscillator tolerance, as with
    // HAL.
    std::uint32_t prescaler = 0;
    std::uint64_t ticks = 0;
    for (; prescaler <= 6; ++prescaler) {
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
    DBGMCU->APB4FZ1 = DBGMCU->APB4FZ1 | DBGMCU_APB4FZ1_DBG_IWDG1;
    IWDG1->KR = 0xcccc;
    started_ = true;
    IWDG1->KR = 0x5555;
    IWDG1->PR = prescaler;
    IWDG1->RLR = static_cast<std::uint32_t>(ticks - 1);
    std::uint32_t budget = kWatchdogUpdateBudget;
    while (IWDG1->SR && --budget) {
      __NOP();
    }
    if (!budget) {
      return core::Status::timeout;
    }
    IWDG1->KR = 0xaaaa;
    return core::Status::ok;
  }

  core::Status Watchdog::Feed() {
    if (!started_) {
      return core::Status::not_running;
    }
    IWDG1->KR = 0xaaaa;
    return core::Status::ok;
  }

  watchdog::Driver Watchdog::driver() {
    return {this,
            [](void* p, core::Time timeout) {
              return static_cast<Watchdog*>(p)->Start(timeout);
            },
            [](void* p) { return static_cast<Watchdog*>(p)->Feed(); }};
  }

  bool prepare_health(const util::Version& version) {
    DBGMCU->APB1LFZ1 = DBGMCU->APB1LFZ1 | DBGMCU_APB1LFZ1_DBG_TIM2;
    set_fault_identity(version, 0);
    const bool watchdog_reset = RCC->RSR & RCC_RSR_IWDG1RSTF;
    RCC->RSR = RCC->RSR | RCC_RSR_RMVF;
    return watchdog_reset;
  }

  [[noreturn]] void initialization_failed() {
    if ((RCC->APB1LENR & RCC_APB1LENR_USART3EN) &&
        (USART3->CR1 & (USART_CR1_UE | USART_CR1_TE)) ==
            (USART_CR1_UE | USART_CR1_TE)) {
      for (char byte :
           std::string_view("DaveOS initialization failed; resetting\r\n")) {
        auto budget = kFailureUartBudget;
        while (!(USART3->ISR & USART_ISR_TXE_TXFNF) && --budget) {
          __NOP();
        }
        if (!budget) {
          break;
        }
        USART3->TDR = static_cast<std::uint8_t>(byte);
      }
    }
    __DSB();
    reset();
  }

  util::fault::Record& retained_fault() {
    return *reinterpret_cast<util::fault::Record*>(0x20000000);
  }

  void set_fault_identity(const util::Version& version,
                          std::uint64_t installation) {
    fault_version = version;
    fault_installation = installation;
  }

  void record_failure(util::fault::Data data) {
    data.version = fault_version;
    data.installation = fault_installation;
    util::fault::Record encoded;
    util::fault::save(encoded, data);
    // Commit complete DTCM words. Subword stores can leave an incomplete SRAM
    // write across reset even after a CPU barrier; publish the magic last.
    auto* destination = reinterpret_cast<volatile std::uint32_t*>(0x20000000);
    destination[0] = 0;
    for (std::size_t i = 1; i < encoded.bytes.size() / 4; ++i) {
      destination[i] = util::wire::read32(encoded.bytes, i * 4);
    }
    __DSB();
    destination[0] = util::wire::read32(encoded.bytes, 0);
    __DSB();
  }

  [[noreturn]] void reset() { NVIC_SystemReset(); }


}  // namespace daveos::platform::stm32h7

extern "C" void DaveOS_FaultCapture(const std::uint32_t* frame,
                                    std::uint32_t exc_return,
                                    std::uint32_t kind) {
  namespace h7 = daveos::platform::stm32h7;
  namespace fault = daveos::util::fault;
  fault::Data data;
  constexpr const char* kFaultNames[] = {"hard_fault", "memory_fault",
                                         "bus_fault", "usage_fault"};
  fault::copy_name(data.check, kind < 4 ? kFaultNames[kind] : "exception");
  fault::copy_name(data.module, "core");
  fault::copy_name(data.task, "exception");
  data.status = static_cast<std::uint32_t>(daveos::core::Status::health_failed);
  data.kind = static_cast<fault::Kind>(kind);
  data.exc_return = exc_return;
  data.stack = reinterpret_cast<std::uintptr_t>(frame);
  data.cfsr = SCB->CFSR;
  data.hfsr = SCB->HFSR;
  data.mmfar = SCB->MMFAR;
  data.bfar = SCB->BFAR;
  constexpr std::uint32_t kStackErrors =
      (1u << 3) | (1u << 4) | (1u << 5) | (1u << 11) | (1u << 12) | (1u << 13);
  auto address = reinterpret_cast<std::uintptr_t>(frame);
  // Core registers come first in both basic and extended frames; optional
  // floating-point storage follows the eight words we retain.
  const auto valid = [](std::uintptr_t value, std::uintptr_t begin,
                        std::uintptr_t end) {
    return value >= begin && value <= end - 32 && !(value & 3);
  };
  if (!(data.cfsr & kStackErrors) && (valid(address, 0x20000800, 0x20020000) ||
                                      valid(address, 0x24000000, 0x24080000) ||
                                      valid(address, 0x30000000, 0x30048000) ||
                                      valid(address, 0x38000000, 0x38010000))) {
    const auto* source =
        reinterpret_cast<const volatile std::uint32_t*>(address);
    for (std::size_t i = 0; i < data.frame.size(); ++i) {
      data.frame[i] = source[i];
    }
    data.frame_valid = true;
  }
  h7::record_failure(data);
  if (CoreDebug->DHCSR & CoreDebug_DHCSR_C_DEBUGEN_Msk) {
    __BKPT(0);
  }
  h7::reset();
}
