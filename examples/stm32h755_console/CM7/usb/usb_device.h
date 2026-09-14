#pragma once

#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool UsbDeviceInit(void);
void UsbDeviceStop(void);
bool UsbDeviceReady(void);
// Call with interrupts masked; storage survives through UsbTransmitComplete.
bool UsbDeviceTransmit(const uint8_t* bytes, uint32_t size);
// Application callbacks, invoked in USB interrupt context.
void UsbReceive(const uint8_t* bytes, uint32_t size);
void UsbTransmitComplete(void);
// Called after TX hardware releases its buffers, on close/reset/disconnect.
void UsbSessionReset(void);
#ifdef __cplusplus
}
#endif
