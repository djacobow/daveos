stm32_usb_console_dep = declare_dependency(
  sources: files('usb.cpp', 'usb_device.c'),
  include_directories: include_directories('.'),
  compile_args: ['-DHAL_PCD_MODULE_ENABLED'],
  dependencies: [stm32_usb_device_dep, console_dep])
