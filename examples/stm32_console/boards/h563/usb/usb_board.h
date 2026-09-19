#pragma once

#include <stdbool.h>

#include "stm32h5xx_hal.h"

#define USB_PRODUCT_NAME "DaveOS H563 console"
#define USB_CRS_SOURCE RCC_CRS_SYNC_SOURCE_USB
#define USB_RESET_ON_SUSPEND 1

// Board-specific controller setup and transfer retirement. The shared CDC
// implementation owns the PCD handle. Abort runs in USB interrupt context.
bool UsbBoardInitPcd(PCD_HandleTypeDef* pcd);
void UsbBoardAbortTransmit(PCD_HandleTypeDef* pcd);
