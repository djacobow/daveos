#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <type_traits>

namespace daveos::core {


  // Non-owning, allocation-free timer callback. Bound objects must remain alive
  // until cancellation/completion and until any in-flight ISR has returned.
  // Identity is the plain function, or the object plus bound member function.
  // Bound identity uses the Invoke specialization's address. Builds must
  // preserve distinct address-taken functions: unsafe linker folding such as
  // --icf=all is unsupported (including for plain function callbacks).
  class TimerCallback {
    template <typename Owner>
    static Owner* OwnerType(void (Owner::*)());
    template <typename Owner>
    static Owner* OwnerType(void (Owner::*)() noexcept);
    template <typename Owner>
    static const Owner* OwnerType(void (Owner::*)() const);
    template <typename Owner>
    static const Owner* OwnerType(void (Owner::*)() const noexcept);

   public:
    constexpr TimerCallback(std::nullptr_t = nullptr) {}

    template <typename F>
      requires std::convertible_to<F, void (*)()>
    constexpr TimerCallback(F function) : plain_(function) {}

    template <auto Function, typename Object>
      requires std::is_member_function_pointer_v<decltype(Function)> &&
               std::is_invocable_r_v<void, decltype(Function), Object&>
    static constexpr TimerCallback bind(Object& object) {
      static_assert(Function != nullptr,
                    "timer member callback cannot be null");
      static_assert(
          std::same_as<std::invoke_result_t<decltype(Function), Object&>, void>,
          "timer callback must return void");
      TimerCallback callback;
      // Normalize to the declaring class: binding through a base or derived
      // reference must produce the same identity, including multiple
      // inheritance.
      using Owner = std::remove_pointer_t<decltype(OwnerType(Function))>;
      Owner* target = &object;
      callback.object_ = target;
      callback.invoke_ = Invoke<Function, Owner>;
      return callback;
    }

    constexpr explicit operator bool() const { return plain_ || invoke_; }

    void operator()() const {
      if (plain_) {
        plain_();
      } else if (invoke_) {
        invoke_(object_);
      }
    }

    friend constexpr bool operator==(const TimerCallback&,
                                     const TimerCallback&) = default;

   private:
    template <auto Function, typename Owner>
    static void Invoke(const void* context) {
      std::invoke(Function,
                  *const_cast<Owner*>(static_cast<const Owner*>(context)));
    }

    void (*plain_)() = nullptr;
    const void* object_ = nullptr;
    void (*invoke_)(const void*) = nullptr;
  };


}  // namespace daveos::core
