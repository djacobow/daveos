firmware_deps = [stm32_console_dep, stm32h5_dep, stm32h5_hal_dep]
if get_option('usb_console')
  subdir('usb')
  firmware_deps += h563_usb_dep
  foreach component : ['pcd', 'pcd_ex', 'll_usb']
    firmware_deps += stm32h5_hal_components[component]
  endforeach
endif
if get_option('networking')
  subdir('ethernet')
  firmware_deps += [stm32h5_hal_components['eth'], stm32h5_hal_components['eth_ex']]
endif
# Keep this selection aligned with CubeMX's enabled peripherals.
foreach component : ['cortex', 'icache', 'rcc', 'rcc_ex', 'flash', 'flash_ex',
                     'gpio', 'dma', 'dma_ex', 'pwr', 'pwr_ex', 'exti', 'uart', 'uart_ex']
  firmware_deps += stm32h5_hal_components[component]
endforeach

board_linker_script = files('STM32H563xx_FLASH.ld')
board_linker_path = meson.current_source_dir() / 'STM32H563xx_FLASH.ld'
board_sources = files(
  'Core/Src/main.c', 'Core/Src/stm32h5xx_it.c',
  'Core/Src/stm32h5xx_hal_msp.c', 'Core/Src/system_stm32h5xx.c',
  'Core/Src/sysmem.c', 'Core/Src/syscalls.c', 'startup_stm32h563xx.s',
  'console.cpp')
board_includes = include_directories('.', 'Core/Inc')
board_args = []
