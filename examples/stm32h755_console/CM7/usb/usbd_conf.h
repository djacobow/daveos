#pragma once

#include <stdint.h>
#include <string.h>

#include "stm32h7xx_hal.h"

#define USBD_MAX_NUM_INTERFACES 2U
#define USBD_MAX_NUM_CONFIGURATION 1U
#define USBD_MAX_SUPPORTED_CLASS 1U
#define USBD_MAX_STR_DESC_SIZ 128U
#define USBD_SELF_POWERED 1U
#define USBD_DEBUG_LEVEL 0U
#define USBD_LPM_ENABLED 0U
#define USBD_SUPPORT_USER_STRING_DESC 0U
#define USBD_memset memset
#define USBD_memcpy memcpy
#define USBD_Delay HAL_Delay
#define USBD_malloc UsbAllocate
#define USBD_free UsbFree
#define USBD_UsrLog(...)
#define USBD_ErrLog(...)
#define USBD_DbgLog(...)
#ifdef __cplusplus
extern "C" {
#endif
// The single CDC class uses one fixed, aligned object; no heap allocator.
void* UsbAllocate(uint32_t size);
void UsbFree(void* pointer);
#ifdef __cplusplus
}
#endif
