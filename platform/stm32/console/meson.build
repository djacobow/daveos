# Sources compile in the application with its board_config.h and HAL settings.
stm32_console_dep = declare_dependency(dependencies: [console_dep, util_dep])
stm32_uart_console_dep = declare_dependency(sources: files('uart.cpp'),
  dependencies: stm32_console_dep)
if 'usb' in features
  subdir('usb')
endif
