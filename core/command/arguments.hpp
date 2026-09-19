#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "core/enum/choices.hpp"
#include "match.hpp"

namespace daveos::core {


  // Views borrow the dispatcher's line buffer until the handler returns.
  using CommandArguments = std::span<const std::string_view>;

  namespace detail {
    struct NoBound {};

    template <auto& Table>
    struct Choices {
      static constexpr auto& entries = Table;
    };

    template <typename E>
    inline constexpr auto EnumChoices = enum_choices(E{});

  }  // namespace detail

  // Fluent, constexpr argument declarations retain bound types for validation.
  template <typename Minimum = detail::NoBound,
            typename Maximum = detail::NoBound, bool Friendly = false,
            typename ChoiceSet = void>
  struct Argument {
    const char* name;
    Minimum minimum{};
    Maximum maximum{};

    template <typename T>
    constexpr auto min(T value) const {
      return Argument<T, Maximum, Friendly, ChoiceSet>{name, value, maximum};
    }

    template <typename T>
    constexpr auto max(T value) const {
      return Argument<Minimum, T, Friendly, ChoiceSet>{name, minimum, value};
    }

    template <typename L, typename H>
    constexpr auto range(L low, H high) const {
      return min(low).max(high);
    }

    // Table must have static constexpr storage. Labels can differ from enum
    // identifiers or select a subset; its enum must match the handler
    // parameter.
    template <auto& Table>
    constexpr auto choices() const {
      return Argument<Minimum, Maximum, Friendly, detail::Choices<Table>>{
          name, minimum, maximum};
    }

    constexpr auto friendly() const {
      return Argument<Minimum, Maximum, true, ChoiceSet>{name, minimum,
                                                         maximum};
    }
  };

  constexpr auto arg(const char* name) { return Argument<>{name}; }

  namespace detail {
    template <typename T>
    struct Optional {
      using Value = T;
      static constexpr bool value = false;
    };

    template <typename T>
    struct Optional<std::optional<T>> {
      using Value = T;
      static constexpr bool value = true;
    };

    // Bounds are converted once, at compile time, without losing 64-bit
    // integers.
    union Bound {
      std::int64_t signed_value;
      std::uint64_t unsigned_value;
      double float_value;

      constexpr Bound() : unsigned_value(0) {}
    };

    template <typename T>
    constexpr void Store(Bound& bound, T value) {
      if constexpr (std::is_floating_point_v<T>) {
        bound.float_value = value;
      } else if constexpr (std::is_signed_v<T>) {
        bound.signed_value = value;
      } else {
        bound.unsigned_value = value;
      }
    }

    template <typename T>
    constexpr T Load(const Bound& bound) {
      if constexpr (std::is_floating_point_v<T>) {
        return static_cast<T>(bound.float_value);
      } else if constexpr (std::is_signed_v<T>) {
        return static_cast<T>(bound.signed_value);
      } else {
        return static_cast<T>(bound.unsigned_value);
      }
    }
  }  // namespace detail

  // Homogeneous descriptors own their metadata; no temporary declaration views.
  // This limit matches the default dispatcher token capacity (which also counts
  // the module and command). Increase it here if larger handlers are needed.
  inline constexpr std::size_t CommandParameterCapacity = 16;

  struct ArgumentMetadata {
    const char* name = nullptr;
    const char* type = nullptr;
    detail::Bound minimum{};
    detail::Bound maximum{};
    bool has_minimum = false;
    bool has_maximum = false;
    bool friendly = false;
    bool optional = false;
    std::size_t choice_count = 0;
    const char* (*choice_name)(std::size_t) = nullptr;
    Status (*parse_choice)(std::string_view, void*) = nullptr;
  };

  // Set only for adapter failures, not for errors returned by a handler.
  struct ArgumentError {
    const ArgumentMetadata* argument = nullptr;
    const char* reason = nullptr;
    Status status = Status::invalid_argument;
  };

  namespace detail {
    template <typename T, typename B>
    constexpr T CheckBound(B value) {
      static_assert(std::is_arithmetic_v<B> && !std::is_same_v<B, bool>,
                    "argument bounds must be numeric");
      if constexpr (std::is_integral_v<T>) {
        static_assert(std::is_integral_v<B>,
                      "integer arguments need integer bounds");
        if constexpr (std::is_integral_v<B>) {
          if (!std::in_range<T>(value)) {
            std::abort();  // Bound is not representable by the handler
                           // parameter.
          }
        }
      } else {
        if (!(value >= std::numeric_limits<T>::lowest() &&
              value <= std::numeric_limits<T>::max())) {
          std::abort();  // Also rejects NaN and infinity at constant
                         // evaluation.
        }
      }
      return static_cast<T>(value);
    }

    template <typename E, auto& Table>
    constexpr void ChoiceMetadata(ArgumentMetadata& result) {
      using Value = std::remove_cvref_t<decltype(Table[0].value)>;
      static_assert(std::is_same_v<E, Value>,
                    "choice table enum must match the handler parameter");
      if (Table.empty()) {
        std::abort();
      }
      for (std::size_t i = 0; i < Table.size(); ++i) {
        if (!Table[i].name || !*Table[i].name) {
          std::abort();
        }
        for (std::size_t j = 0; j < i; ++j) {
          if (EqualName(Table[i].name, Table[j].name)) {
            std::abort();
          }
        }
      }
      result.type = "enum choice";
      result.choice_count = Table.size();
      result.choice_name = [](std::size_t i) { return Table[i].name; };
      result.parse_choice = [](std::string_view text, void* output) {
        auto match = lazy_match(text, Table.size(),
                                [](std::size_t i) { return Table[i].name; });
        if (match.status == Status::ok) {
          *static_cast<E*>(output) = Table[match.index].value;
        }
        return match.status;
      };
    }

    template <typename P, typename L, typename H, bool F, typename C>
    constexpr ArgumentMetadata Metadata(Argument<L, H, F, C> declaration) {
      using T = typename Optional<P>::Value;
      static_assert(std::is_enum_v<T> || std::is_integral_v<T> ||
                        std::is_same_v<T, float> || std::is_same_v<T, double> ||
                        std::is_same_v<T, std::string_view>,
                    "unsupported command parameter type");
      static_assert(!F || std::is_same_v<T, bool>,
                    "friendly requires a boolean parameter");
      static_assert(std::is_same_v<C, void> || std::is_enum_v<T>,
                    "choices require an enum parameter");
      constexpr bool low = !std::is_same_v<L, NoBound>;
      constexpr bool high = !std::is_same_v<H, NoBound>;
      static_assert(!(low || high) ||
                        (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>),
                    "bounds require a numeric parameter");
      if (!declaration.name || !*declaration.name) {
        std::abort();
      }
      ArgumentMetadata result{};
      result.name = declaration.name;
      result.optional = Optional<P>::value;
      result.friendly = F;
      result.has_minimum = low;
      result.has_maximum = high;
      if constexpr (std::is_enum_v<T>) {
        if constexpr (!std::is_same_v<C, void>) {
          ChoiceMetadata<T, C::entries>(result);
        } else {
          static_assert(
              requires { enum_choices(T{}); },
              "enum parameters need DAVEOS_ENUM or an explicit choices table");
          if constexpr (requires { enum_choices(T{}); }) {
            ChoiceMetadata<T, EnumChoices<T>>(result);
          }
        }
      } else if constexpr (std::is_same_v<T, bool>) {
        result.type = F ? "boolean alias" : "true or false";
      } else if constexpr (std::is_integral_v<T>) {
        result.type =
            std::is_signed_v<T> ? "signed integer" : "unsigned integer";
      } else if constexpr (std::is_floating_point_v<T>) {
        result.type = "finite floating-point number";
      } else {
        result.type = "text";
      }
      if constexpr (low) {
        Store(result.minimum, CheckBound<T>(declaration.minimum));
      }
      if constexpr (high) {
        Store(result.maximum, CheckBound<T>(declaration.maximum));
      }
      if constexpr (low && high) {
        if (Load<T>(result.minimum) > Load<T>(result.maximum)) {
          std::abort();
        }
      }
      return result;
    }

    template <typename T>
    bool Number(std::string_view text, T& value) {
      if (text.empty()) {
        return false;
      }
      if constexpr (std::is_integral_v<T>) {
        bool negative = text.front() == '-';
        if (negative || text.front() == '+') {
          text.remove_prefix(1);
        }
        if (negative && !std::is_signed_v<T>) {
          return false;
        }
        int base = 10;
        if (text.size() >= 2 && text[0] == '0') {
          if (text[1] == 'x' || text[1] == 'X') {
            base = 16;
            text.remove_prefix(2);
          } else if (text[1] == 'b' || text[1] == 'B') {
            base = 2;
            text.remove_prefix(2);
          }
        }
        if (text.empty() || text.front() == '-' || text.front() == '+') {
          return false;
        }
        std::uint64_t magnitude = 0;
        auto end = text.data() + text.size();
        auto parsed = std::from_chars(text.data(), end, magnitude, base);
        if (parsed.ec != std::errc{} || parsed.ptr != end) {
          return false;
        }
        auto maximum =
            static_cast<std::uint64_t>(std::numeric_limits<T>::max());
        if constexpr (std::is_signed_v<T>) {
          if (negative) {
            if (magnitude > maximum + 1) {
              return false;
            }
            value = magnitude == maximum + 1
                        ? std::numeric_limits<T>::min()
                        : static_cast<T>(-static_cast<std::int64_t>(magnitude));
            return true;
          }
        }
        if (magnitude > maximum) {
          return false;
        }
        value = static_cast<T>(magnitude);
        return true;
      } else {
        // from_chars is locale independent and requires no NUL-terminated copy.
        if (text.front() == '+') {
          text.remove_prefix(1);
          if (!text.empty() && text.front() == '-') {
            return false;
          }
        }
        if (text.empty() || text.front() == '+') {
          return false;
        }
        auto end = text.data() + text.size();
        auto parsed = std::from_chars(text.data(), end, value,
                                      std::chars_format::general);
        return parsed.ec == std::errc{} && parsed.ptr == end &&
               std::isfinite(value);
      }
    }

    template <typename P>
    Status Parse(std::string_view text, const ArgumentMetadata& metadata,
                 P& output) {
      using T = typename Optional<P>::Value;
      T value{};
      if constexpr (std::is_enum_v<T>) {
        auto status = metadata.parse_choice(text, &value);
        if (status != Status::ok) {
          return status;
        }
      } else if constexpr (std::is_same_v<T, bool>) {
        if (!metadata.friendly) {
          if (text != "true" && text != "false") {
            return Status::invalid_argument;
          }
          value = text == "true";
        } else {
          constexpr std::array yes{"true",   "1",    "on", "yes",
                                   "enable", "high", "set"};
          constexpr std::array no{"false",   "0",   "off",  "no",
                                  "disable", "low", "clear"};
          bool found = false;
          for (std::size_t i = 0; i < yes.size(); ++i) {
            if (EqualName(text, yes[i]) || EqualName(text, no[i])) {
              value = EqualName(text, yes[i]);
              found = true;
              break;
            }
          }
          if (!found) {
            return Status::invalid_argument;
          }
        }
      } else if constexpr (std::is_same_v<T, std::string_view>) {
        value = text;
      } else {
        if (!Number(text, value) ||
            (metadata.has_minimum && value < Load<T>(metadata.minimum)) ||
            (metadata.has_maximum && value > Load<T>(metadata.maximum))) {
          return Status::invalid_argument;
        }
      }
      output = value;
      return Status::ok;
    }

    // GCC/Clang expose the selected function in their template signature. This
    // is only a label for direct factory users; type validation never relies
    // on parsing compiler text. The macro supplies its own exact identifier.
    template <auto Function>
    consteval auto HandlerName() {
      constexpr std::string_view signature = __PRETTY_FUNCTION__;
      constexpr auto start = signature.find("Function = ") + 11;
      constexpr auto end = signature.find_first_of(";]", start);
      constexpr auto qualified = signature.substr(start, end - start);
      constexpr auto scope = qualified.rfind("::");
      constexpr auto name =
          qualified.substr(scope == std::string_view::npos ? 0 : scope + 2);
      std::array<char, name.size() + 1> result{};
      for (std::size_t i = 0; i < name.size(); ++i) {
        result[i] = name[i];
      }
      return result;
    }

    template <auto Function>
    inline constexpr auto HandlerLabel = HandlerName<Function>();

    template <typename T>
    struct Signature;

    template <typename R, typename M, typename... P>
    struct Signature<R (M::*)(P...)> {
      using Owner = M;
      using Result = R;
      using Parameters = std::tuple<P...>;
    };

    template <typename R, typename M, typename... P>
    struct Signature<R (M::*)(P...) const> : Signature<R (M::*)(P...)> {};

    template <typename R, typename M, typename... P>
    struct Signature<R (M::*)(P...) noexcept> : Signature<R (M::*)(P...)> {};

    template <typename R, typename M, typename... P>
    struct Signature<R (M::*)(P...) const noexcept>
        : Signature<R (M::*)(P...)> {};
  }  // namespace detail

  template <typename M>
  struct CommandDescriptor {
    const char* name;
    const char* help;
    Status (*callback)(M&, CommandArguments, const CommandDescriptor&,
                       ArgumentError&);
    const char* handler;
    std::array<ArgumentMetadata, CommandParameterCapacity> arguments{};
    std::size_t count = 0;
    std::size_t required = 0;
  };

  namespace detail {
    template <auto Function, typename Tuple, std::size_t... I>
    Status Invoke(
        typename Signature<decltype(Function)>::Owner& owner,
        CommandArguments args,
        const CommandDescriptor<typename Signature<decltype(Function)>::Owner>&
            descriptor,
        ArgumentError& error, std::index_sequence<I...>) {
      Tuple values{};
      [[maybe_unused]] auto parse = [&]<std::size_t Index>() {
        if (Index >= args.size()) {
          return true;
        }
        auto status = Parse(args[Index], descriptor.arguments[Index],
                            std::get<Index>(values));
        if (status == Status::ok) {
          return true;
        }
        error = {&descriptor.arguments[Index],
                 status == Status::ambiguous_match ? "ambiguous choice"
                 : status == Status::not_found
                     ? "unknown choice"
                     : "invalid value or outside allowed range",
                 status};
        return false;
      };
      if (!(parse.template operator()<I>() && ...)) {
        return error.status;
      }
      return std::apply(
          [&](auto... value) { return (owner.*Function)(value...); }, values);
    }
  }  // namespace detail

  // Build a homogeneous descriptor and an allocation-free typed invocation
  // thunk.
  template <auto Function, typename... A>
  consteval auto command(const char* name, const char* help, A... arguments) {
    using S = detail::Signature<decltype(Function)>;
    using M = typename S::Owner;
    using P = typename S::Parameters;
    static_assert(std::is_same_v<typename S::Result, Status>,
                  "command handler must return Status");
    constexpr bool raw = std::is_same_v<P, std::tuple<CommandArguments>>;
    constexpr auto count = std::tuple_size_v<P>;
    static_assert(raw ? sizeof...(A) == 0 : sizeof...(A) == count,
                  "one argument descriptor is required per handler parameter");
    static_assert(raw || count <= CommandParameterCapacity,
                  "too many command parameters");
    CommandDescriptor<M> result{name, help, nullptr,
                                detail::HandlerLabel<Function>.data()};
    if constexpr (raw) {
      result.callback = [](M& owner, CommandArguments args,
                           const CommandDescriptor<M>&,
                           ArgumentError&) { return (owner.*Function)(args); };
    } else if constexpr (sizeof...(A) == count &&
                         count <= CommandParameterCapacity) {
      [&]<std::size_t... I>(std::index_sequence<I...>) {
        ((result.arguments[I] =
              detail::Metadata<std::tuple_element_t<I, P>>(arguments)),
         ...);
      }
      (std::make_index_sequence<count>{});
      result.count = count;
      bool optional = false;
      for (std::size_t i = 0; i < count; ++i) {
        if (result.arguments[i].optional) {
          optional = true;
        } else if (optional) {
          std::abort();  // Required parameters cannot follow optional ones.
        } else {
          ++result.required;
        }
      }
      result.callback = [](M& owner, CommandArguments args,
                           const CommandDescriptor<M>& descriptor,
                           ArgumentError& error) {
        if (args.size() < descriptor.required ||
            args.size() > descriptor.count) {
          error.reason = "wrong argument count";
          return Status::invalid_argument;
        }
        return detail::Invoke<Function, P>(owner, args, descriptor, error,
                                           std::make_index_sequence<count>{});
      };
    }
    return result;
  }

// Capture the C++ identifier without declaring the handler or its parameters.
#define DAVEOS_COMMAND(ModuleType, function, name, description, ...)  \
  []() consteval {                                                    \
    auto descriptor = ::daveos::core::command<&ModuleType::function>( \
        name, description __VA_OPT__(, ) __VA_ARGS__);                \
    descriptor.handler = #function;                                   \
    return descriptor;                                                \
  }                                                                   \
  ()


}  // namespace daveos::core
