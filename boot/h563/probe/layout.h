#pragma once

#include "boot/flash.h"

namespace daveos::boot {


  // Sizing-only constants; this probe must never be programmed. The final
  // layout is generated from its ELF load size and checked again after linking.
  inline constexpr Layout kLayout{{0x08004000, 0x08104000},
                                  {0x08002000, 0x08102000},
                                  0xfc000,
                                  8192,
                                  16,
                                  0x563,
                                  1};


}  // namespace daveos::boot
