#pragma once

#include <array>
#include <type_traits>

namespace daveos::core {


  // Static command spelling and its actual enum value; values need not be
  // dense.
  template <typename E>
  struct EnumChoice {
    static_assert(std::is_enum_v<E>);
    const char* name;
    E value;
  };


}  // namespace daveos::core
