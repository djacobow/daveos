#pragma once

#include <algorithm>
#include <chrono>

#include "hal/spi/action.hpp"
#include "protocol.h"
#include "storage/block_device.h"

namespace daveos::storage::sd {


  // Read-only CMD17 adapter for an initialized SDHC/SDXC card. Does not own
  // initialization, bus speed or CS configuration. rate must be the actual
  // SCK rate, <=1 MHz. Caller supplies a capture buffer and a cooperative pump.
  // Pump returns false to request abort; an accepted transfer ALWAYS finishes
  // (including its HAL timeout) before buffers are released. Pump must keep
  // making progress even after requesting abort; fake platforms advance there.
  // No ISR calls. One execution thread owns the reader and its scratch buffer.
  class Reader {
   public:
    static constexpr std::uint32_t kMaximumHz = 1000000;
    static constexpr std::size_t kCaptureBytes =
        kMaximumHz / 80 + 8 + 1 + kSectorBytes + 2 + 1;

    struct Pump {
      void* context;
      bool (*run)(void*);
    };

    Reader(const hal::spi::Device& device, std::span<std::uint8_t> capture,
           std::uint32_t rate, std::uint64_t sectors, const Pump& pump)
        : device_(device),
          capture_(capture),
          rate_(rate),
          sectors_(sectors),
          pump_(pump) {}

    bool read(std::uint32_t sector, std::span<std::uint8_t> destination) {
      if (busy_ || !pump_.run || !rate_ || rate_ > kMaximumHz ||
          destination.empty() || destination.size() % kSectorBytes ||
          std::uint64_t{sector} + destination.size() / kSectorBytes >
              sectors_ ||
          std::uint64_t{sector} + destination.size() / kSectorBytes >
              (std::uint64_t{1} << 32)) {
        return false;
      }
      const auto wait_bytes = rate_ / 80 + 1;
      const auto receive_size = 8 + wait_bytes + kSectorBytes + 2;
      if (capture_.size() < receive_size) {
        return false;
      }
      busy_ = true;

      struct Release {
        bool& busy;

        ~Release() { busy = false; }
      } release{busy_};

      if (!pump_.run(pump_.context)) {
        return false;
      }
      auto wire = capture_.first(receive_size);
      for (std::size_t offset = 0; offset < destination.size();
           offset += kSectorBytes) {
        const auto tx = command(17, sector++);
        std::fill(wire.begin(), wire.end(), 0xff);
        const std::array actions{hal::spi::write(tx), hal::spi::read(wire)};
        if (!Transfer(actions)) {
          return false;
        }
        auto payload = data(wire, kSectorBytes, wait_bytes);
        if (!payload) {
          return false;
        }
        std::copy(payload->begin(), payload->end(),
                  destination.begin() + offset);
        const std::array gap{hal::spi::idle_clocks(8)};
        if (!Transfer(gap)) {
          return false;
        }
      }
      return true;
    }

   private:
    bool Transfer(std::span<const hal::spi::Action> actions) {
      if (completion_.start(device_, actions, std::chrono::milliseconds{250}) !=
          hal::Status::ok) {
        return false;
      }
      bool keep_going = true;
      while (!completion_.ready()) {
        if (!pump_.run(pump_.context)) {
          keep_going = false;
        }
      }
      return keep_going && completion_.result()->status == hal::Status::ok;
    }

    hal::spi::Device device_;
    std::span<std::uint8_t> capture_;
    std::uint32_t rate_;
    std::uint64_t sectors_;
    Pump pump_;
    hal::spi::Completion completion_;
    bool busy_ = false;
  };


}  // namespace daveos::storage::sd
