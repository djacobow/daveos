#pragma once

#include <algorithm>
#include <chrono>

#include "hal/spi/action.hpp"
#include "protocol.h"
#include "read.h"
#include "storage/block_device.h"

namespace daveos::storage::sd {


  // CMD17/CMD24 adapter for an initialized SDHC/SDXC card. Does not own
  // initialization, bus speed or CS configuration; use Initializer first. rate
  // must be the actual SCK rate, <=1 MHz.
  //
  // Admission: read()/write() reject bad ranges, sizes and reentry without
  //   touching the bus; those rejections leave the transport usable.
  // Ownership: the caller's capture buffer and data stay borrowed until the
  //   call returns; an accepted transfer ALWAYS finishes (including its HAL
  //   timeout) before they are released.
  // Execution: synchronous on one execution thread, never from an ISR. The
  //   injected pump runs while a transfer is pending; it returns false to
  //   request abort but must keep making progress (fake platforms advance
  //   time there).
  // Deadline: 250 ms per read transaction; writes allow 1 s including busy
  //   polling, then require clean CMD13 status.
  // Failure: an accepted transfer failure or bad card response latches
  //   failed(); no further I/O from this instance. No automatic retry.
  // Lifetime: reconstruct only after explicit card reinitialization.
  class Transport {
   public:
    static constexpr std::uint32_t kMaximumHz = 1000000;
    static constexpr std::size_t kCaptureBytes = kSectorBytes + 4;

    struct WriteDiagnostics {
      hal::Status status = hal::Status::ok;
      std::uint32_t command = 24;
      std::size_t completed_actions = 0;
      std::array<std::uint8_t, 8> response{}, accepted{};
      std::array<std::uint8_t, 8> ready{};
      std::array<std::uint8_t, 10> card_status{};
    };

    const WriteDiagnostics& write_diagnostics() const { return diagnostics_; }

    bool failed() const { return failed_; }

    hal::Status status() const { return diagnostics_.status; }

    struct Pump {
      void* context;
      bool (*run)(void*);
    };

    Transport(const hal::spi::Device& device, std::span<std::uint8_t> capture,
              std::uint32_t rate, std::uint64_t sectors, const Pump& pump)
        : device_(device),
          capture_(capture),
          rate_(rate),
          sectors_(sectors),
          pump_(pump) {}

    bool read(std::uint32_t sector, std::span<std::uint8_t> destination) {
      if (failed_ || busy_ || !pump_.run || !rate_ || rate_ > kMaximumHz ||
          destination.empty() || destination.size() % kSectorBytes ||
          std::uint64_t{sector} + destination.size() / kSectorBytes >
              sectors_ ||
          std::uint64_t{sector} + destination.size() / kSectorBytes >
              (std::uint64_t{1} << 32)) {
        return false;
      }
      const auto receive_size = kCaptureBytes;
      if (capture_.size() < receive_size) {
        return false;
      }
      busy_ = true;
      operation_started_ = false;

      struct Release {
        bool& busy;

        ~Release() { busy = false; }
      } release{busy_};

      if (!pump_.run(pump_.context)) {
        diagnostics_.status = hal::Status::aborted;
        return false;
      }
      auto wire = capture_.first(receive_size);
      for (std::size_t offset = 0; offset < destination.size();
           offset += kSectorBytes) {
        const auto tx = command(17, sector++);
        std::fill(wire.begin(), wire.end(), 0xff);
        const auto actions = read_actions(tx, wire);
        if (!Transfer(actions)) {
          return false;
        }
        auto payload = data(wire, kSectorBytes, 1);
        if (!payload) {
          return Fail(hal::Status::response_mismatch);
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

    // Write one sector at a time. A bounded response check prevents payload
    // transmission after a rejected CMD24. Hold CS through data/response and
    // bounded response polling, then require ready and
    // clean CMD13 status before reporting success. No automatic write retry.
    bool write(std::uint32_t sector, std::span<const std::uint8_t> source) {
      if (failed_ || busy_ || !pump_.run || !rate_ || rate_ > kMaximumHz ||
          source.empty() || source.size() % kSectorBytes ||
          std::uint64_t{sector} + source.size() / kSectorBytes > sectors_ ||
          std::uint64_t{sector} + source.size() / kSectorBytes >
              (std::uint64_t{1} << 32)) {
        return false;
      }
      busy_ = true;
      operation_started_ = false;

      struct Release {
        bool& busy;

        ~Release() { busy = false; }
      } release{busy_};

      if (!pump_.run(pump_.context)) {
        diagnostics_.status = hal::Status::aborted;
        return false;
      }
      diagnostics_ = {};
      for (std::size_t offset = 0; offset < source.size();
           offset += kSectorBytes) {
        const auto tx = command(24, sector++);
        const auto bytes = source.subspan(offset, kSectorBytes);
        const auto crc = crc16(bytes);
        const std::array<std::uint8_t, 2> header{0xff, 0xfe};
        const std::array<std::uint8_t, 2> trailer{
            static_cast<std::uint8_t>(crc >> 8),
            static_cast<std::uint8_t>(crc)};
        diagnostics_.command = 24;
        auto& response = diagnostics_.response;
        auto& accepted = diagnostics_.accepted;
        auto& ready = diagnostics_.ready;
        response.fill(0xff);
        accepted.fill(0xff);
        const std::array actions{hal::spi::write(tx),
                                 hal::spi::read(response),
                                 hal::spi::check_response(response, 0),
                                 hal::spi::write(header),
                                 hal::spi::write(bytes),
                                 hal::spi::write(trailer),
                                 hal::spi::read(accepted),
                                 hal::spi::check_response(accepted, 5, 0x1f),
                                 hal::spi::poll_response(ready, 0xff)};
        if (!Transfer(actions, kWriteTimeout) || !Gap()) {
          return false;
        }
        const auto status_command = command(13, 0);
        diagnostics_.command = 13;
        auto& status = diagnostics_.card_status;
        status.fill(0xff);
        const std::array status_actions{hal::spi::write(status_command),
                                        hal::spi::read(status)};
        if (!Transfer(status_actions) || !Gap()) {
          return false;
        }
        const auto first =
            std::find_if(status.begin(), status.end(),
                         [](auto value) { return value != 0xff; });
        if (first == status.end() || first + 1 == status.end() || *first != 0 ||
            first[1] != 0) {
          return Fail(hal::Status::response_mismatch);
        }
      }
      return true;
    }

   private:
    static constexpr auto kWriteTimeout = std::chrono::milliseconds{1000};

    bool Fail(hal::Status status) {
      failed_ = true;
      diagnostics_.status = status;
      return false;
    }

    bool Gap() {
      const std::array actions{hal::spi::idle_clocks(8)};
      return Transfer(actions);
    }

    bool Transfer(std::span<const hal::spi::Action> actions,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds{
                      250}) {
      const auto start = completion_.start(device_, actions, timeout);
      if (start != hal::Status::ok) {
        diagnostics_.status = start;
        if (operation_started_ || start == hal::Status::faulted ||
            start == hal::Status::not_initialized) {
          failed_ = true;
        }
        return false;
      }
      operation_started_ = true;
      bool keep_going = true;
      while (!completion_.ready()) {
        if (!pump_.run(pump_.context)) {
          keep_going = false;
        }
      }
      diagnostics_.status = completion_.result()->status;
      diagnostics_.completed_actions = completion_.result()->completed_actions;
      if (diagnostics_.status != hal::Status::ok) {
        return Fail(diagnostics_.status);
      }
      // Even a completed transfer may be just one part of the card protocol.
      // A caller abandoning that protocol must initialize the card again.
      return keep_going || Fail(hal::Status::aborted);
    }

    WriteDiagnostics diagnostics_;
    hal::spi::Device device_;
    std::span<std::uint8_t> capture_;
    std::uint32_t rate_;
    std::uint64_t sectors_;
    Pump pump_;
    hal::spi::Completion completion_;
    bool busy_ = false, failed_ = false, operation_started_ = false;
  };


}  // namespace daveos::storage::sd
