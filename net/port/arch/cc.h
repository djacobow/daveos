#pragma once

#include <inttypes.h>
#include <stdlib.h>
#define LWIP_NO_UNISTD_H 1
#define LWIP_PLATFORM_DIAG(x) \
  do {                        \
  } while (0)
#define LWIP_PLATFORM_ASSERT(x) abort()
