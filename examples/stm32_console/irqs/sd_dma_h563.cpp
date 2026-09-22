// GPDMA1 channels 1/2 carry SD payloads for the sd-dma feature on H563.
#include "components.hpp"

extern "C" void GPDMA1_Channel1_IRQHandler() { app::components.sd.interrupt(); }

extern "C" void GPDMA1_Channel2_IRQHandler() { app::components.sd.interrupt(); }
