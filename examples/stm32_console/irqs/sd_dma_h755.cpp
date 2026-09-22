// DMA1 streams 1/2 carry SD payloads for the sd-dma feature on H755.
#include "components.hpp"

extern "C" void DMA1_Stream1_IRQHandler() { app::components.sd.interrupt(); }

extern "C" void DMA1_Stream2_IRQHandler() { app::components.sd.interrupt(); }
