#pragma once

#include <optional>
#include <span>

#include "hal/status.h"

namespace daveos::platform::stm32::detail {


  // Optional SPI DMA engine. DMA accesses only engine-owned staging storage,
  // never caller buffers. start copies TX; read copies RX only after stop has
  // proved the engine quiescent. A failed stop faults the controller, but
  // cannot leave a DMA master accessing released application buffers.
  struct SpiDma {
    void* context = nullptr;
    hal::Status (*init)(void*) = nullptr;
    hal::Status (*start)(void*, std::span<const std::uint8_t>, std::size_t,
                         std::uint8_t) = nullptr;
    std::optional<hal::Status> (*poll)(void*) = nullptr;
    bool (*stop)(void*) = nullptr;
    void (*read)(void*, std::span<std::uint8_t>) = nullptr;
    std::size_t capacity = 0;

    explicit operator bool() const {
      return init && start && poll && stop && read && capacity;
    }
  };


}  // namespace daveos::platform::stm32::detail
