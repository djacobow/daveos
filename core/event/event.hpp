#pragma once

#include <cstddef>
#include <cstdlib>
#include <tuple>
#include <type_traits>
#include <variant>

namespace daveos::core {


  // Default event domain for applications that do not exchange events.
  using NoEvent = std::variant<std::monostate>;

  namespace detail {
    template <typename T, typename... Types>
    inline constexpr std::size_t TypeCount =
        (std::size_t{0} + ... + std::size_t(std::is_same_v<T, Types>));

    template <typename T>
    struct EventVariant {
      static constexpr bool valid = false;
      template <typename>
      static constexpr bool contains = false;
    };

    template <typename... Payloads>
    struct EventVariant<std::variant<Payloads...>> {
      static constexpr bool valid =
          ((TypeCount<Payloads, Payloads...> == 1 &&
            std::is_trivially_copyable_v<Payloads> &&
            std::is_nothrow_default_constructible_v<Payloads> &&
            std::is_nothrow_copy_constructible_v<Payloads> &&
            std::is_nothrow_copy_assignable_v<Payloads>)&&...) &&
          std::is_trivially_copyable_v<std::variant<Payloads...>>;
      template <typename T>
      static constexpr bool contains = TypeCount<T, Payloads...> == 1;
    };

    template <typename T>
    struct EventMember {
      static_assert(
          !std::is_same_v<T, T>,
          "event handler must return void and take exactly const Payload&");
    };

    template <typename M, typename P>
    struct EventMember<void (M::*)(const P&)> {
      using Owner = M;
      using Payload = P;
    };

    template <typename M, typename P>
    struct EventMember<void (M::*)(const P&) const>
        : EventMember<void (M::*)(const P&)> {};

    template <typename M, typename P>
    struct EventMember<void (M::*)(const P&) noexcept>
        : EventMember<void (M::*)(const P&)> {};

    template <typename M, typename P>
    struct EventMember<void (M::*)(const P&) const noexcept>
        : EventMember<void (M::*)(const P&)> {};
  }  // namespace detail

  // Variants and alternatives are copied into fixed queue slots. Payloads must
  // own their data or independently guarantee the lifetime of borrowed storage.
  template <typename T>
  concept EventType = detail::EventVariant<T>::valid;

  template <typename Payload, typename Event>
  concept EventAlternative = detail::EventVariant<Event>::template
  contains<Payload>;

  // Each descriptor retains its callback's exact type; events() returns a tuple
  // so handlers for different payloads need no runtime type erasure.
  template <auto Function>
  struct EventHandler {
    static_assert(Function != nullptr, "event handler must not be null");
    using Owner = typename detail::EventMember<decltype(Function)>::Owner;
    using Payload = typename detail::EventMember<decltype(Function)>::Payload;
    static constexpr auto callback = Function;
    const char* name;
  };

  template <auto Function>
  consteval auto event_handler(const char* name) {
    if (!name || !*name) {
      std::abort();
    }
    return EventHandler<Function>{name};
  }

  namespace detail {
    template <typename M, typename Event, typename... Handlers>
    consteval bool ValidEventHandlers(const std::tuple<Handlers...>& handlers) {
      static_assert((std::is_same_v<typename Handlers::Owner, M> && ...),
                    "event handlers must belong to the registered module");
      static_assert(
          (EventAlternative<typename Handlers::Payload, Event> && ...),
          "event handler payload must be an alternative of the application "
          "variant");
      static_assert(((TypeCount<typename Handlers::Payload,
                                typename Handlers::Payload...> == 1) &&
                     ...),
                    "register at most one handler per payload type");
      return std::apply(
          [](const auto&... handler) {
            return ((handler.name && *handler.name) && ...);
          },
          handlers);
    }
  }  // namespace detail

// Name the handler once, preserving its C++ identifier for log attribution.
#define DAVEOS_EVENT(ModuleType, function) \
  ::daveos::core::event_handler<&ModuleType::function>(#function)


}  // namespace daveos::core
