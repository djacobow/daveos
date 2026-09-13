#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <utility>

#include "daveos/core/platform.h"

namespace app {


struct TxCounters {
  std::uint32_t dropped_frames = 0;
  std::uint32_t errors = 0;
  std::uint64_t sent_bytes = 0;
  std::uint32_t transfers = 0;
};
// Task-side write()/flush() collect a complete display/log frame, then copy it
// atomically into the filling half of caller-owned DMA-accessible storage.
// The other half belongs exclusively to DMA until completion. Never wait for
// space. Driver::start(bytes, size) starts one asynchronous transfer and
// returns bool. Its completion/error handlers call complete()/error() after
// hardware releases the buffer. P serializes task and IRQ access; neither
// callback may run inline from start(). All objects/storage must outlive
// outstanding transfers.
template <typename P, typename Driver, std::size_t Capacity,
          std::size_t FrameCapacity = 768>
class DmaOutput {
  static_assert(Capacity > 0 && Capacity <= 65535 && FrameCapacity > 0);

 public:
  DmaOutput(P& platform, Driver& driver,
            std::array<std::uint8_t, 2 * Capacity>& storage)
      : platform_(platform), driver_(driver), storage_(storage) {}
  void write(std::string_view text) {
    if (overflow_) return;
    if (text.size() > frame_.size() - frame_size_) {
      overflow_ = true;
      return;
    }
    std::copy(text.begin(), text.end(), frame_.begin() + frame_size_);
    frame_size_ += text.size();
  }
  daveos::core::Status flush() {
    daveos::core::Guard guard(platform_);
    auto size = std::exchange(frame_size_, 0);
    if (std::exchange(overflow_, false) || size > Capacity - pending_) {
      ++counters_.dropped_frames;
      return daveos::core::Status::full;
    }
    std::copy_n(frame_.begin(), size,
                storage_.begin() + filling_ * Capacity + pending_);
    pending_ += size;
    return Start() ? daveos::core::Status::ok
                   : daveos::core::Status::initialization_failed;
  }
  void complete() {
    daveos::core::Guard guard(platform_);
    if (!active_) return;
    counters_.sent_bytes += active_;
    ++counters_.transfers;
    active_ = 0;
    Start();
  }
  // A failed transfer has uncertain progress. Discard the queued tail rather
  // than sending a fragment of an old frame; subsequent frames can start
  // afresh.
  void error() {
    daveos::core::Guard guard(platform_);
    ++counters_.errors;
    pending_ = active_ = 0;
  }
  TxCounters counters() {
    daveos::core::Guard guard(platform_);
    return counters_;
  }
  std::size_t queued() {
    daveos::core::Guard guard(platform_);
    return pending_ + active_;
  }

 private:
  bool Start() {
    if (active_ || !pending_) return true;
    if (driver_.start(storage_.data() + filling_ * Capacity, pending_)) {
      active_ = std::exchange(pending_, 0);
      filling_ ^= 1;
      return true;
    }
    error();
    return false;
  }
  P& platform_;
  Driver& driver_;
  std::array<std::uint8_t, 2 * Capacity>& storage_;
  std::array<char, FrameCapacity> frame_{};
  std::size_t frame_size_ = 0;
  bool overflow_ = false;
  std::size_t filling_ = 0, pending_ = 0, active_ = 0;
  TxCounters counters_{};
};


}  // namespace app
