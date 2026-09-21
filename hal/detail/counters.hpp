#pragma once

#include <array>
#include <atomic>
#include <limits>

#include "hal/transaction.hpp"

namespace daveos::hal::detail {


  // Host uses lock-free 64-bit atomics. On 32-bit MCUs the supplied critical
  // domain masks interrupts for one counter access, avoiding libatomic locks.
  // Snapshot fields are synchronized samples, not an atomic multi-field epoch.
  template <typename Critical>
  class Counters {
    static constexpr bool kAtomic =
        std::atomic<std::uint64_t>::is_always_lock_free;
    using Cell =
        std::conditional_t<kAtomic, std::atomic<std::uint64_t>, std::uint64_t>;

   public:
    enum Index : std::size_t {
      read_attempts,
      write_attempts,
      read_errors,
      write_errors,
      accepted,
      completed,
      failed,
      timed_out,
      rejected,
      cleanup_failures,
      resets,
      reset_failures,
      count
    };

    void increment(Index index, Critical& critical) {
      auto& cell = cells_[index];
      if constexpr (kAtomic) {
        auto value = cell.load(std::memory_order_relaxed);
        while (value != std::numeric_limits<std::uint64_t>::max() &&
               !cell.compare_exchange_weak(value, value + 1,
                                           std::memory_order_relaxed)) {
        }
      } else {
        critical.enter();
        if (cell != std::numeric_limits<std::uint64_t>::max()) {
          ++cell;
        }
        critical.leave();
      }
    }

    Statistics snapshot(Critical& critical, bool clear = false) {
      std::array<std::uint64_t, count> values{};
      for (std::size_t i = 0; i < count; ++i) {
        if constexpr (kAtomic) {
          values[i] = clear ? cells_[i].exchange(0) : cells_[i].load();
        } else {
          critical.enter();
          values[i] = cells_[i];
          if (clear) {
            cells_[i] = 0;
          }
          critical.leave();
        }
      }
      return {values[0], values[1], values[2],  values[3],
              values[4], values[5], values[6],  values[7],
              values[8], values[9], values[10], values[11]};
    }

   private:
    std::array<Cell, count> cells_{};
  };


}  // namespace daveos::hal::detail
