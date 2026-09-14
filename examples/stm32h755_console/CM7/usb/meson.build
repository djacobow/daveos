h755_usb_dep = declare_dependency(
  sources: files('usb_device.c', 'usb.cpp'),
  include_directories: include_directories('.'),
  compile_args: ['-DDAVEOS_USB_CDC=1', '-DHAL_PCD_MODULE_ENABLED'],
  dependencies: stm32h7_usb_device_dep)
