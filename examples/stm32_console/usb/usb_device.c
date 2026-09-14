#include "usb_device.h"

#include "usbd_cdc.h"
#include "usbd_core.h"
#include "usbd_ctlreq.h"

static PCD_HandleTypeDef pcd;
static USBD_HandleTypeDef device;
static USBD_CDC_HandleTypeDef class_storage;
static bool allocated;
static bool opened;
static uint8_t rx_buffer[CDC_DATA_FS_MAX_PACKET_SIZE];
static uint8_t line_coding[7] = {0x40, 0x42, 0x0f, 0, 0, 0, 8};

void* UsbAllocate(uint32_t size) {
  if (allocated || size > sizeof(class_storage)) return NULL;
  allocated = true;
  memset(&class_storage, 0, sizeof(class_storage));
  return &class_storage;
}
void UsbFree(void* pointer) {
  if (pointer == &class_storage) allocated = false;
}
static void CloseSession(void) {
  opened = false;
  UsbBoardAbortTransmit(&pcd);
  class_storage.TxState = 0;
  UsbSessionReset();
}
static int8_t CdcInit(void) {
  opened = false;
  USBD_CDC_SetTxBuffer(&device, NULL, 0);
  USBD_CDC_SetRxBuffer(&device, rx_buffer);
  return USBD_OK;
}
static int8_t CdcDeInit(void) {
  opened = false;
  UsbSessionReset();  // CDC has already closed its endpoints.
  return USBD_OK;
}
static int8_t Control(uint8_t command, uint8_t* bytes, uint16_t size) {
  switch (command) {
    case CDC_SET_LINE_CODING:
      if (size != sizeof(line_coding)) return USBD_FAIL;
      memcpy(line_coding, bytes, sizeof(line_coding));
      break;
    case CDC_GET_LINE_CODING:
      if (size != sizeof(line_coding)) return USBD_FAIL;
      memcpy(bytes, line_coding, sizeof(line_coding));
      break;
    case CDC_SET_CONTROL_LINE_STATE: {
      USBD_SetupReqTypedef* request = (USBD_SetupReqTypedef*)bytes;
      bool dtr = (request->wValue & 1U) != 0;
      if (!dtr) CloseSession();
      opened = dtr;
      break;
    }
    default:
      break;
  }
  return USBD_OK;
}
static int8_t Receive(uint8_t* bytes, uint32_t* size) {
  if (opened) UsbReceive(bytes, *size);
  USBD_CDC_SetRxBuffer(&device, rx_buffer);
  return (int8_t)USBD_CDC_ReceivePacket(&device);
}
static int8_t Transmitted(uint8_t* bytes, uint32_t* size, uint8_t endpoint) {
  (void)bytes;
  (void)size;
  (void)endpoint;
  UsbTransmitComplete();
  return USBD_OK;
}
static USBD_CDC_ItfTypeDef interface = {CdcInit, CdcDeInit, Control, Receive,
                                        Transmitted};

// ST's example VID/PID, for this ST-board development demo only.
static uint8_t descriptor[] = {18,   USB_DESC_TYPE_DEVICE,
                               0,    2,
                               2,    2,
                               0,    64,
                               0x83, 0x04,
                               0x40, 0x57,
                               0,    1,
                               1,    2,
                               3,    1};
static uint8_t language[] = {4, USB_DESC_TYPE_STRING, 0x09, 0x04};
static uint8_t string_buffer[USBD_MAX_STR_DESC_SIZ];
static uint8_t* DeviceDescriptor(USBD_SpeedTypeDef speed, uint16_t* size) {
  (void)speed;
  *size = sizeof(descriptor);
  return descriptor;
}
static uint8_t* Language(USBD_SpeedTypeDef speed, uint16_t* size) {
  (void)speed;
  *size = sizeof(language);
  return language;
}
static uint8_t* String(const char* text, uint16_t* size) {
  USBD_GetString((uint8_t*)text, string_buffer, size);
  return string_buffer;
}
static uint8_t* Manufacturer(USBD_SpeedTypeDef speed, uint16_t* size) {
  (void)speed;
  return String("DaveOS", size);
}
static uint8_t* Product(USBD_SpeedTypeDef speed, uint16_t* size) {
  (void)speed;
  return String(USB_PRODUCT_NAME, size);
}
static uint8_t* Serial(USBD_SpeedTypeDef speed, uint16_t* size) {
  (void)speed;
  // Stable per-chip ID, formatted in hex without any libc integer conversion.
  const uint32_t words[] = {HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2()};
  char serial[25];
  const char hex[] = "0123456789ABCDEF";
  for (uint32_t i = 0; i < 24; ++i)
    serial[i] = hex[(words[i / 8] >> (28 - (i % 8) * 4)) & 15];
  serial[24] = 0;
  return String(serial, size);
}
static uint8_t* Configuration(USBD_SpeedTypeDef speed, uint16_t* size) {
  (void)speed;
  return String("CDC console", size);
}
static USBD_DescriptorsTypeDef descriptors = {
    DeviceDescriptor, Language,      Manufacturer, Product,
    Serial,           Configuration, Configuration};

bool UsbDeviceReady(void) {
  return opened && device.dev_state == USBD_STATE_CONFIGURED;
}
bool UsbDeviceTransmit(const uint8_t* bytes, uint32_t size) {
  if (!UsbDeviceReady() || class_storage.TxState) return false;
  USBD_CDC_SetTxBuffer(&device, (uint8_t*)bytes, size);
  return USBD_CDC_TransmitPacket(&device) == USBD_OK;
}
bool UsbDeviceInit(void) {
  RCC_OscInitTypeDef oscillator = {0};
  oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI48;
  oscillator.HSI48State = RCC_HSI48_ON;
  oscillator.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) return false;
  RCC_PeriphCLKInitTypeDef clocks = {0};
  clocks.PeriphClockSelection = RCC_PERIPHCLK_USB;
  clocks.UsbClockSelection = RCC_USBCLKSOURCE_HSI48;
  if (HAL_RCCEx_PeriphCLKConfig(&clocks) != HAL_OK) return false;
  __HAL_RCC_CRS_CLK_ENABLE();
  RCC_CRSInitTypeDef crs = {0};
  crs.Prescaler = RCC_CRS_SYNC_DIV1;
  crs.Source = USB_CRS_SOURCE;
  crs.Polarity = RCC_CRS_SYNC_POLARITY_RISING;
  crs.ReloadValue = 47999;
  crs.ErrorLimitValue = RCC_CRS_ERRORLIMIT_DEFAULT;
  crs.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;
  HAL_RCCEx_CRSConfig(&crs);
  if (USBD_Init(&device, &descriptors, 0) != USBD_OK) return false;
  if (USBD_RegisterClass(&device, USBD_CDC_CLASS) != USBD_OK) return false;
  if (USBD_CDC_RegisterInterface(&device, &interface) != USBD_OK) return false;
  return USBD_Start(&device) == USBD_OK;
}
void UsbDeviceStop(void) {
  // Clock setup can fail before the middleware has a low-level handle.
  if (device.pData == NULL) return;
  USBD_Stop(&device);
  USBD_DeInit(&device);
  device.pData = NULL;
}
void UsbDeviceInterrupt(void) { HAL_PCD_IRQHandler(&pcd); }
USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef* dev) {
  pcd.pData = dev;
  dev->pData = &pcd;
  return UsbBoardInitPcd(&pcd) ? USBD_OK : USBD_FAIL;
}
static USBD_StatusTypeDef Status(HAL_StatusTypeDef status) {
  return status == HAL_OK ? USBD_OK : USBD_FAIL;
}
USBD_StatusTypeDef USBD_LL_DeInit(USBD_HandleTypeDef* dev) {
  return Status(HAL_PCD_DeInit(dev->pData));
}
USBD_StatusTypeDef USBD_LL_Start(USBD_HandleTypeDef* dev) {
  return Status(HAL_PCD_Start(dev->pData));
}
USBD_StatusTypeDef USBD_LL_Stop(USBD_HandleTypeDef* dev) {
  return Status(HAL_PCD_Stop(dev->pData));
}
USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef* dev, uint8_t ep,
                                  uint8_t type, uint16_t size) {
  return Status(HAL_PCD_EP_Open(dev->pData, ep, size, type));
}
USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef* dev, uint8_t ep) {
  return Status(HAL_PCD_EP_Close(dev->pData, ep));
}
USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef* dev, uint8_t ep) {
  return Status(HAL_PCD_EP_Flush(dev->pData, ep));
}
USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef* dev, uint8_t ep) {
  return Status(HAL_PCD_EP_SetStall(dev->pData, ep));
}
USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef* dev, uint8_t ep) {
  return Status(HAL_PCD_EP_ClrStall(dev->pData, ep));
}
uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef* dev, uint8_t ep) {
  PCD_HandleTypeDef* handle = dev->pData;
  return (ep & 0x80) ? handle->IN_ep[ep & 0x7f].is_stall
                     : handle->OUT_ep[ep].is_stall;
}
USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef* dev,
                                         uint8_t address) {
  return Status(HAL_PCD_SetAddress(dev->pData, address));
}
USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef* dev, uint8_t ep,
                                    uint8_t* bytes, uint32_t size) {
  return Status(HAL_PCD_EP_Transmit(dev->pData, ep, bytes, size));
}
USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef* dev, uint8_t ep,
                                          uint8_t* bytes, uint32_t size) {
  return Status(HAL_PCD_EP_Receive(dev->pData, ep, bytes, size));
}
uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef* dev, uint8_t ep) {
  return HAL_PCD_EP_GetRxCount(dev->pData, ep);
}
void USBD_LL_Delay(uint32_t delay) { HAL_Delay(delay); }
void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef* h) {
  USBD_LL_SetupStage(h->pData, (uint8_t*)h->Setup);
}
void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef* h, uint8_t ep) {
  USBD_LL_DataOutStage(h->pData, ep, h->OUT_ep[ep].xfer_buff);
}
void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef* h, uint8_t ep) {
  USBD_LL_DataInStage(h->pData, ep, h->IN_ep[ep].xfer_buff);
}
void HAL_PCD_SOFCallback(PCD_HandleTypeDef* h) { USBD_LL_SOF(h->pData); }
void HAL_PCD_ResetCallback(PCD_HandleTypeDef* h) {
  USBD_LL_SetSpeed(h->pData, USBD_SPEED_FULL);
  USBD_LL_Reset(h->pData);
  opened = false;
  UsbSessionReset();
}
void HAL_PCD_SuspendCallback(PCD_HandleTypeDef* h) {
  USBD_LL_Suspend(h->pData);
#if USB_RESET_ON_SUSPEND
  // DRD FS has no VBUS disconnect interrupt. Suspend also covers cable loss.
  // Retain DTR across a normal host resume, but discard stale session buffers.
  const bool was_open = opened;
  CloseSession();
  opened = was_open;
#endif
}
void HAL_PCD_ResumeCallback(PCD_HandleTypeDef* h) { USBD_LL_Resume(h->pData); }
void HAL_PCD_ConnectCallback(PCD_HandleTypeDef* h) {
  USBD_LL_DevConnected(h->pData);
}
void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef* h) {
  CloseSession();
  USBD_LL_DevDisconnected(h->pData);
}
void HAL_PCD_ISOOUTIncompleteCallback(PCD_HandleTypeDef* h, uint8_t ep) {
  USBD_LL_IsoOUTIncomplete(h->pData, ep);
}
void HAL_PCD_ISOINIncompleteCallback(PCD_HandleTypeDef* h, uint8_t ep) {
  USBD_LL_IsoINIncomplete(h->pData, ep);
}
