#include "usb_device.h"

#include "usb_board.h"
#include "usbd_cdc.h"

void OTG_FS_IRQHandler(void) { UsbDeviceInterrupt(); }

void HAL_PCD_MspInit(PCD_HandleTypeDef* handle) {
  (void)handle;
  HAL_PWREx_EnableUSBVoltageDetector();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  GPIO_InitTypeDef pins = {0};
  pins.Pin = GPIO_PIN_11 | GPIO_PIN_12;
  pins.Mode = GPIO_MODE_AF_PP;
  pins.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  pins.Alternate = GPIO_AF10_OTG2_FS;
  HAL_GPIO_Init(GPIOA, &pins);
  pins.Pin = GPIO_PIN_9;
  pins.Mode = GPIO_MODE_INPUT;
  HAL_GPIO_Init(GPIOA, &pins);
  __HAL_RCC_USB2_OTG_FS_CLK_ENABLE();
  __HAL_RCC_USB2_OTG_FS_CLK_SLEEP_ENABLE();
  __HAL_RCC_USB2_OTG_FS_ULPI_CLK_SLEEP_DISABLE();
  HAL_NVIC_SetPriority(OTG_FS_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(OTG_FS_IRQn);
}

void HAL_PCD_MspDeInit(PCD_HandleTypeDef* handle) {
  (void)handle;
  HAL_NVIC_DisableIRQ(OTG_FS_IRQn);
  __HAL_RCC_USB2_OTG_FS_CLK_DISABLE();
  HAL_GPIO_DeInit(GPIOA, GPIO_PIN_9 | GPIO_PIN_11 | GPIO_PIN_12);
}

bool UsbBoardInitPcd(PCD_HandleTypeDef* pcd) {
  pcd->Instance = USB2_OTG_FS;
  pcd->Init.dev_endpoints = 6;
  pcd->Init.speed = PCD_SPEED_FULL;
  pcd->Init.phy_itface = PCD_PHY_EMBEDDED;
  pcd->Init.vbus_sensing_enable = ENABLE;
  pcd->Init.dma_enable = DISABLE;  // FIFO interrupt transfers; DTCM is safe.
  pcd->Init.low_power_enable = DISABLE;
  if (HAL_PCD_Init(pcd) != HAL_OK) return false;
  if (HAL_PCDEx_SetRxFiFo(pcd, 128) != HAL_OK ||
      HAL_PCDEx_SetTxFiFo(pcd, 0, 64) != HAL_OK ||
      HAL_PCDEx_SetTxFiFo(pcd, 1, 96) != HAL_OK ||
      HAL_PCDEx_SetTxFiFo(pcd, 2, 16) != HAL_OK)
    return false;
  return true;
}

void UsbBoardAbortTransmit(PCD_HandleTypeDef* pcd) {
  // Retire FIFO service and pending completion before releasing buffer storage.
  const uint32_t USBx_BASE = (uint32_t)pcd->Instance;
  const uint32_t endpoint = CDC_IN_EP & 0x7fU;
  USBx_DEVICE->DIEPEMPMSK &= ~(1UL << endpoint);
  HAL_PCD_EP_Abort(pcd, CDC_IN_EP);
  HAL_PCD_EP_Flush(pcd, CDC_IN_EP);
  USBx_INEP(endpoint)->DIEPINT = USB_OTG_DIEPINT_XFRC | USB_OTG_DIEPINT_EPDISD;
}
