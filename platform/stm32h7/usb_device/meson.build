usb_root = '../STM32CubeH7/Middlewares/ST/STM32_USB_Device_Library'
stm32h7_usb_device_dep = declare_dependency(
  sources: files(usb_root / 'Core/Src/usbd_core.c',
                 usb_root / 'Core/Src/usbd_ctlreq.c',
                 usb_root / 'Core/Src/usbd_ioreq.c',
                 usb_root / 'Class/CDC/Src/usbd_cdc.c'),
  include_directories: include_directories(usb_root / 'Core/Inc', usb_root / 'Class/CDC/Inc'))
