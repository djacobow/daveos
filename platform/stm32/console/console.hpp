#pragma once

#include <tuple>

#include "core/command/source.hpp"

namespace daveos::platform::stm32 {


  // Borrow any selection of console modules, including none. The group owns no
  // hardware or callbacks. Declare the modules before use, keep them alive, and
  // stop the group before stopping their network/platform dependencies.
  template <typename... Transports>
  class Console {
   public:
    explicit Console(Transports&... transports) : transports_(&transports...) {}

    template <typename... Other>
    auto modules(Other&... other) {
      return std::apply(
          [&](auto*... transport) {
            return core::ModuleList{&other..., transport...};
          },
          transports_);
    }

    auto subscribers() {
      return std::apply(
          [](auto*... transport) {
            return core::SubscriberList{transport->subscriber()...};
          },
          transports_);
    }

    auto sources() {
      return std::apply(
          [](auto*... transport) {
            return core::CommandSourceList{transport->command_source()...};
          },
          transports_);
    }

    template <typename Event>
    void log_statistics(core::SchedulerInterface<Event>& scheduler) {
      std::apply(
          [&](auto*... transport) { (Report(*transport, scheduler), ...); },
          transports_);
    }

    // Reverse registration order, with no scheduler lifecycle side effects.
    void stop() { Stop(std::index_sequence_for<Transports...>{}); }

   private:
    template <typename T, typename Event>
    static void Report(T& transport,
                       core::SchedulerInterface<Event>& scheduler) {
      if constexpr (requires { transport.log_statistics(scheduler); }) {
        transport.log_statistics(scheduler);
      }
    }

    template <std::size_t... I>
    void Stop(std::index_sequence<I...>) {
      (std::get<sizeof...(I) - 1 - I>(transports_)->stop(), ...);
    }

    std::tuple<Transports*...> transports_;
  };

  template <typename... Transports>
  Console(Transports&...) -> Console<Transports...>;


}  // namespace daveos::platform::stm32
