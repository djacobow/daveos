// I2C1 event/error interrupts for the i2c-adc feature (H563 PB8/PB9).
#include "components.hpp"

extern "C" void I2C1_EV_IRQHandler() { app::components.i2c1.interrupt(); }

extern "C" void I2C1_ER_IRQHandler() { app::components.i2c1.interrupt(); }
