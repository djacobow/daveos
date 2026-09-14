# Compile shared behavior against the target's board_config.h.
stm32_console_dep = declare_dependency(sources: files('console.cpp'),
  compile_args: ['-DDAVEOS_TCP_CONSOLE=' + (get_option('tcp_console') ? '1' : '0'),
    '-DDAVEOS_UART_CONSOLE=' + (get_option('uart_console') ? '1' : '0')])
if get_option('usb_console')
  subdir('usb')
endif
