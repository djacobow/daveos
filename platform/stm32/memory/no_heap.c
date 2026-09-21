#include <errno.h>
#include <stddef.h>
#include <stdint.h>

/* Debugger-visible evidence of attempted allocation, including constructors.
 * Never log here: formatting may itself have requested the allocation. */
volatile uint32_t daveos_heap_attempts;
volatile int32_t daveos_heap_last_request;

void *_sbrk(ptrdiff_t increment) {
  uint32_t mask;
  __asm volatile("mrs %0, primask\n cpsid i" : "=r"(mask) : : "memory");
  if (daveos_heap_attempts != UINT32_MAX) {
    ++daveos_heap_attempts;
  }
  daveos_heap_last_request = increment;
  errno = ENOMEM;
  __asm volatile("msr primask, %0" : : "r"(mask) : "memory");
  return (void *)-1;
}
