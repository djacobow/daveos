firmware_deps = [stm32_console_dep, stm32h7_dep, stm32h7_hal_dep]
if get_option('usb_console')
  subdir('usb')
  firmware_deps += h755_usb_dep
  foreach component : ['pcd', 'pcd_ex', 'll_usb']
    firmware_deps += stm32h7_hal_components[component]
  endforeach
endif
if get_option('networking')
  subdir('ethernet')
  firmware_deps += [stm32h7_hal_components['eth'], stm32h7_hal_components['eth_ex']]
endif
foreach component : ['cortex', 'hsem', 'rcc', 'rcc_ex', 'flash', 'flash_ex',
                     'gpio', 'dma', 'dma_ex', 'pwr', 'pwr_ex', 'exti', 'uart', 'uart_ex']
  firmware_deps += stm32h7_hal_components[component]
endforeach
board_linker_script = files('stm32h755xx_flash_CM7.ld')
board_linker_path = meson.current_source_dir() / 'stm32h755xx_flash_CM7.ld'
board_sources = files(
  'Core/Src/main.c', 'Core/Src/stm32h7xx_it.c',
  'Core/Src/stm32h7xx_hal_msp.c', 'Core/Src/syscalls.c',
  '../Common/Src/system_stm32h7xx_dualcore_boot_cm4_cm7.c',
  'Core/Startup/startup_stm32h755xx_CM7.s', 'console.cpp', '../../../../stm32h7/fault_handlers.S')
board_includes = include_directories('.', 'Core/Inc')
board_args = ['-DCORE_CM7', '-DUSE_PWR_DIRECT_SMPS_SUPPLY', '-DUSER_VECT_TAB_ADDRESS']
