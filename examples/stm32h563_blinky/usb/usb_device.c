#include "usb_device.h"

#include "usb_board.h"
#include "usbd_cdc.h"

void USB_DRD_FS_IRQHandler(void) { UsbDeviceInterrupt(); }

void HAL_PCD_MspInit(PCD_HandleTypeDef* pcd) {
  (void)pcd;
  // Keep the reset-state USB-C sink termination on CC1/CC2. No PD stack or
  // source power switch is needed for this self-powered USB device demo.
  HAL_PWREx_EnableUCPDDeadBattery();
  HAL_PWREx_EnableVddUSB();
  __HAL_RCC_USB_CLK_ENABLE();
  __HAL_RCC_USB_CLK_SLEEP_ENABLE();
  // PA11/PA12 are dedicated USB pins on H563; no GPIO alternate function.
  HAL_NVIC_SetPriority(USB_DRD_FS_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(USB_DRD_FS_IRQn);
}
void HAL_PCD_MspDeInit(PCD_HandleTypeDef* pcd) {
  (void)pcd;
  HAL_NVIC_DisableIRQ(USB_DRD_FS_IRQn);
  __HAL_RCC_USB_CLK_DISABLE();
}
bool UsbBoardInitPcd(PCD_HandleTypeDef* pcd) {
  pcd->Instance = USB_DRD_FS;
  pcd->Init.dev_endpoints = 8;
  pcd->Init.speed = USBD_FS_SPEED;
  pcd->Init.phy_itface = PCD_PHY_EMBEDDED;
  pcd->Init.low_power_enable = DISABLE;
  pcd->Init.vbus_sensing_enable = DISABLE;
  pcd->Init.bulk_doublebuffer_enable = DISABLE;
  if (HAL_PCD_Init(pcd) != HAL_OK) return false;
  // Single-buffer PMA allocations, each aligned to a 64-byte packet boundary.
  // EP0 OUT/IN, CDC data IN/OUT, CDC notification IN; well within 2 KiB PMA.
  return HAL_PCDEx_PMAConfig(pcd, 0x00, PCD_SNG_BUF, 0x40) == HAL_OK &&
         HAL_PCDEx_PMAConfig(pcd, 0x80, PCD_SNG_BUF, 0x80) == HAL_OK &&
         HAL_PCDEx_PMAConfig(pcd, CDC_IN_EP, PCD_SNG_BUF, 0xc0) == HAL_OK &&
         HAL_PCDEx_PMAConfig(pcd, CDC_OUT_EP, PCD_SNG_BUF, 0x100) == HAL_OK &&
         HAL_PCDEx_PMAConfig(pcd, CDC_CMD_EP, PCD_SNG_BUF, 0x140) == HAL_OK;
}
void UsbBoardAbortTransmit(PCD_HandleTypeDef* pcd) {
  HAL_PCD_EP_Abort(pcd, CDC_IN_EP);
  // Retire a pending completion before the shared output buffer is reused.
  PCD_CLEAR_TX_EP_CTR(pcd->Instance, CDC_IN_EP & 0x7fU);
  pcd->IN_ep[CDC_IN_EP & 0x7fU].xfer_count = 0;
}
