#pragma once

#include <cstdint>

// Define at namespace scope using one X-macro list, then undefine the list:
//   #define APP_COLORS(X) X(red) X(green, 7) X(blue)
//   DAVEOS_ENUM(Color, std::uint32_t, APP_COLORS)
//   #undef APP_COLORS
// This generates an enum class and an ADL-visible constexpr enum_name(Color).
// Names are static, null-terminated strings without the enum type prefix.
// Unrecognized values return "unknown". Explicit values may be sparse or
// negative; aliases with duplicate values are rejected by the generated switch.
// No allocation, runtime initialization, or sentinel enumerator is introduced.
#define DAVEOS_DETAIL_ENUM_VALUE(name, ...) name __VA_OPT__(= __VA_ARGS__),
#define DAVEOS_DETAIL_ENUM_CASE(name, ...) \
  case DaveosEnum::name:                   \
    return #name;
#define DAVEOS_ENUM(Type, Underlying, Values)                        \
  enum class Type : Underlying { Values(DAVEOS_DETAIL_ENUM_VALUE) }; \
  [[nodiscard]] constexpr const char* enum_name(Type value) {        \
    using DaveosEnum = Type;                                         \
    switch (value) {                                                 \
      Values(DAVEOS_DETAIL_ENUM_CASE) default : return "unknown";    \
    }                                                                \
  }
