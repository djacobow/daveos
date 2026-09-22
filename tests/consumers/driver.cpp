// A standalone injected driver: no platform contract, scheduler or logging.
#include "drivers/mcp3425.h"

int main() {
  // An unbound device rejects work; construction and request stay passive.
  daveos::hal::i2c::Device device{};
  daveos::drivers::Mcp3425 adc(device);
  return adc.busy() ? 1 : 0;
}
