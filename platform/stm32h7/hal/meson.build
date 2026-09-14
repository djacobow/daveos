# Source dependencies are compiled in each target with its own HAL configuration.
hal_root = '../STM32CubeH7/Drivers/STM32H7xx_HAL_Driver'
stm32h7_hal_dep = declare_dependency(
  sources: files(hal_root / 'Src/stm32h7xx_hal.c'),
  include_directories: include_directories(hal_root / 'Inc', hal_root / 'Inc/Legacy'),
  compile_args: ['-DUSE_HAL_DRIVER'],
  dependencies: stm32h7_cmsis_dep,
)
stm32h7_hal_components = {}
foreach component : ['eth', 'eth_ex', 'cortex', 'hsem', 'rcc', 'rcc_ex', 'flash', 'flash_ex',
                     'pcd', 'pcd_ex', 'll_usb', 'gpio', 'dma', 'dma_ex', 'pwr', 'pwr_ex', 'exti', 'uart', 'uart_ex']
  stm32h7_hal_components += {component: declare_dependency(
    sources: files(hal_root / 'Src/stm32h7xx_' + (component == 'll_usb' ? '' : 'hal_') + component + '.c'))}
endforeach
