#pragma once

#include <cinttypes>

#include "application.h"
#include "drivers/mcp3425.h"
#include "i2c_probe.hpp"

namespace app {


  // ADC behavior borrows the I2C module's device and task-time lease. Hardware
  // setup, interrupts and bus diagnostics belong to I2cBus.
  class AdcProbe : public core::Module<AdcProbe, Event> {
   public:
    AdcProbe(Platform& platform, I2cBus& i2c)
        : platform_(platform), i2c_(i2c), adc_(I2cBus::device(i2c)) {}

    static constexpr const char* name() { return "adc"; }

    static constexpr auto tasks() {
      return std::array{
          DAVEOS_PERIODIC(AdcProbe, Tick, std::chrono::milliseconds{1})};
    }

    static constexpr auto commands() {
      return std::array{DAVEOS_COMMAND(AdcProbe, Sample, "sample",
                                       "Read MCP3425: 16-bit, gain 1")};
    }

    core::Status Sample() {
      if (pending_ || !i2c_.acquire()) {
        return core::Status::busy;
      }
      if (adc_.request() != daveos::hal::Status::ok) {
        i2c_.release();
        return core::Status::busy;
      }
      pending_ = true;
      return core::Status::ok;
    }

    void Tick() {
      adc_.tick(platform_.now());
      if (pending_) {
        if (const auto result = adc_.result()) {
          pending_ = false;
          i2c_.release();
          if (result->status == daveos::hal::Status::ok) {
            I_("MCP3425: raw=%" PRId32 " voltage=%" PRId32 " uV",
               static_cast<std::int32_t>(result->code), result->microvolts);
          } else {
            E_("MCP3425: %s", enum_name(result->status));
          }
        }
      }
    }

   private:
    Platform& platform_;
    I2cBus& i2c_;
    daveos::drivers::Mcp3425 adc_;
    bool pending_ = false;
  };


}  // namespace app
