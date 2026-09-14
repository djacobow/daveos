# Compile shared behavior against the target's board_config.h.
stm32_console_dep = declare_dependency(sources: files('console.cpp'),
  compile_args: ['-DDAVEOS_UART_CONSOLE=' + (get_option('uart_console') ? '1' : '0')])
