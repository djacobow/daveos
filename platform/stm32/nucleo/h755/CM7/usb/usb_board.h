#pragma once

#include <stdbool.h>

#include "stm32h7xx_hal.h"

#define USB_PRODUCT_NAME "DaveOS H755 console"
#define USB_CRS_SOURCE RCC_CRS_SYNC_SOURCE_USB2
#define USB_RESET_ON_SUSPEND 0

// Board-specific controller setup and transfer retirement. The shared CDC
// implementation owns the PCD handle. Abort runs in USB interrupt context.
bool UsbBoardInitPcd(PCD_HandleTypeDef* pcd);
void UsbBoardAbortTransmit(PCD_HandleTypeDef* pcd);
