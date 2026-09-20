#include <cinttypes>
#include <cstdarg>

#include "boot/control.h"
#include "core/logging/log_format.hpp"
#include "core/logging/logger.hpp"
#include "layout.h"
#include "platform/stm32h5/crc32.h"
#include "platform/stm32h5/reliability.h"
#include "stm32h563xx.h"

namespace {
  namespace core = daveos::core;
  namespace boot = daveos::boot;
  namespace h5 = daveos::platform::stm32h5;
  volatile std::uint32_t milliseconds = 0;
  constexpr core::Time kNoImageDelay = 1000000;
  constexpr std::uint32_t kUartWaitBudget = 640000;

  class Runtime {
   public:
    core::Time now() const { return core::Time{milliseconds} * 1000; }

    bool in_interrupt() const { return __get_IPSR() != 0; }

    core::Context context() const { return context_; }

    void context(core::Context value) { context_ = value; }

    void notify() {}

    void enter() {
      mask_ = __get_PRIMASK();
      __disable_irq();
    }

    void leave() { __set_PRIMASK(mask_); }

   private:
    std::uint32_t mask_ = 0;
    core::Context context_{"boot", "main"};
  } runtime;

  void Uart(std::string_view text) {
    for (char byte : text) {
      std::uint32_t budget = kUartWaitBudget;
      while (!(USART3->ISR & USART_ISR_TXE_TXFNF) && --budget) {
        __NOP();
      }
      if (!budget) {
        return;
      }
      USART3->TDR = static_cast<std::uint8_t>(byte);
    }
  }

  core::Subscriber sink{nullptr, [](void*, const core::LogRecord& record) {
                          core::LogPrefix prefix(record);
                          Uart(prefix.view());
                          Uart(record.message);
                          Uart("\r\n");
                        }};
  auto logger = core::make_logger<2, 128>(runtime, core::SubscriberList{sink});
  h5::Flash flash{&runtime,
                  [](void* p) { return static_cast<Runtime*>(p)->now(); }};
  daveos::util::crc32::Service crc{h5::crc32_backend()};

  void Log(core::Level level, const char* format, ...)
      __attribute__((format(printf, 2, 3)));

  void Log(core::Level level, const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    logger.write(level, format, args);
    va_end(args);
    logger.flush();
  }

  void Initialize() {
    SystemCoreClockUpdate();
    SysTick_Config(SystemCoreClock / 1000);
    __enable_irq();
    RCC->AHB2ENR = RCC->AHB2ENR | RCC_AHB2ENR_GPIODEN;
    RCC->APB1LENR = RCC->APB1LENR | RCC_APB1LENR_USART3EN;
    (void)RCC->APB1LENR;
    // Establish the UART idle level before connecting its alternate function.
    // Selecting AF while the transmitter is disabled creates a false start bit.
    GPIOD->PUPDR = (GPIOD->PUPDR & ~(3u << 16)) | (1u << 16);
    GPIOD->AFR[1] = (GPIOD->AFR[1] & ~15u) | 7u;
    USART3->CR1 = 0;
    USART3->PRESC = 0;
    USART3->BRR = SystemCoreClock / 1000000;
    USART3->CR1 = USART_CR1_TE | USART_CR1_UE;
    GPIOD->MODER = (GPIOD->MODER & ~(3u << 16)) | (2u << 16);
    SCB->SHCSR = SCB->SHCSR | SCB_SHCSR_MEMFAULTENA_Msk |
                 SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk;
  }

  [[noreturn]] __attribute__((naked)) void Enter(std::uint32_t, std::uint32_t) {
    __asm volatile("msr msp, r0\n cpsie i\n bx r1\n");
  }

  [[noreturn]] void Jump(std::uint32_t address) {
    auto* vectors = reinterpret_cast<const std::uint32_t*>(address);
    auto stack = vectors[0], entry = vectors[1];
    std::uint32_t budget = kUartWaitBudget;
    while (!(USART3->ISR & USART_ISR_TC) && --budget) {
      __NOP();
    }
    __disable_irq();
    SysTick->CTRL = 0;
    SysTick->VAL = 0;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
    for (std::uint32_t i = 0; i <= (SCnSCB->ICTR & 15); ++i) {
      NVIC->ICER[i] = 0xffffffff;
      NVIC->ICPR[i] = 0xffffffff;
    }
    USART3->CR1 = 0;
    SCB->VTOR = address;
    __set_CONTROL(0);
    __set_BASEPRI(0);
    __set_FAULTMASK(0);
    __set_MSPLIM(0);
    __DSB();
    __ISB();
    Enter(stack, entry);
  }
}  // namespace

extern "C" void SysTick_Handler() { milliseconds = milliseconds + 1; }

extern "C" void NMI_Handler() {
  if (!h5::handle_flash_ecc()) {
    DaveOS_FaultCapture(nullptr, 0, 0);
  }
}

extern "C" int main() {
  Initialize();
  const daveos::util::Version version{BOOT_VERSION_MAJOR, BOOT_VERSION_MINOR};
  h5::set_fault_identity(version, 0);
  Log(core::Level::info, "DaveOS bootloader %" PRIu32 ".%" PRIu32,
      version.major, version.minor);
  boot::Control control(flash.driver(), boot::kLayout, crc);
  auto selection = control.select();
  if (selection.status == core::Status::ok) {
    const auto address = boot::kLayout.slots[selection.slot];
    const auto* vectors = reinterpret_cast<const std::uint32_t*>(address);
    if (vectors[0] > 0x20000800 && vectors[0] <= 0x200a0000 &&
        !(vectors[0] & 7) && (vectors[1] & 1) &&
        (vectors[1] & ~1u) >= address &&
        (vectors[1] & ~1u) - address < selection.image.size) {
      Log(core::Level::info, "Boot slot %c (%s), CRC verified",
          'A' + static_cast<int>(selection.slot),
          selection.trial ? "trial" : "confirmed");
      Jump(address);
    }
    selection.status = core::Status::incompatible;
  }
  Log(core::Level::error, "No bootable image: %s; resetting",
      enum_name(selection.status));
  // Preserve any application failure record across fallback/no-image boots.
  daveos::util::fault::Data previous;
  if (!daveos::util::fault::read(h5::retained_fault(), previous)) {
    previous.kind = daveos::util::fault::Kind::boot;
    previous.status = static_cast<std::uint32_t>(selection.status);
    previous.version = version;
    daveos::util::fault::save(h5::retained_fault(), previous);
  }
  auto start = runtime.now();
  while (runtime.now() - start < kNoImageDelay) {
    __NOP();
  }
  h5::reset();
}

// Bootloader has no heap; libc formatting failures remain best effort.
extern "C" void* _sbrk(std::ptrdiff_t) {
  return reinterpret_cast<void*>(UINTPTR_MAX);
}
