# Source dependencies are compiled in each target with its own HAL configuration.
hal_root = '../../../third_party/STM32CubeH5/Drivers/STM32H5xx_HAL_Driver'
stm32h5_hal_dep = declare_dependency(
  sources: files(hal_root / 'Src/stm32h5xx_hal.c'),
  include_directories: include_directories(hal_root / 'Inc', hal_root / 'Inc/Legacy'),
  compile_args: ['-DUSE_HAL_DRIVER'],
  dependencies: stm32h5_cmsis_dep,
)
stm32h5_hal_components = {}
foreach component : ['eth', 'eth_ex', 'cortex', 'icache', 'rcc', 'rcc_ex', 'flash', 'flash_ex',
                     'pcd', 'pcd_ex', 'll_usb', 'gpio', 'dma', 'dma_ex', 'pwr', 'pwr_ex', 'exti', 'uart', 'uart_ex']
  stm32h5_hal_components += {component: declare_dependency(
    sources: files(hal_root / 'Src/stm32h5xx_' + (component == 'll_usb' ? '' : 'hal_') + component + '.c'))}
endforeach
