#pragma once

#include "core/schedule/module.hpp"

namespace app {


  namespace core = daveos::core;
  enum class Event {};

  // Application logic is identical on the real-time host and fake platform.
  class Counter final : public core::Module<Counter, Event> {
   public:
    static constexpr const char* name() { return "counter"; }

    static constexpr auto tasks() {
      return std::array{
          DAVEOS_PERIODIC(Counter, Tick, std::chrono::milliseconds{1})};
    }

    std::uint32_t count() const { return count_; }

    core::Status status() const { return status_; }

   private:
    void Tick() {
      if (++count_ == 3) {
        status_ = scheduler().stop();
      }
    }

    std::uint32_t count_ = 0;
    core::Status status_ = core::Status::ok;
  };


}  // namespace app
