// SPI1 interrupt for the sd feature.
#include "components.hpp"

extern "C" void SPI1_IRQHandler() { app::components.sd.interrupt(); }
