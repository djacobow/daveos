cube = '../../platform/stm32h5/STM32CubeH5'
hal = cube / 'Drivers/STM32H5xx_HAL_Driver'
firmware_includes = include_directories(
  'Core/Inc', hal / 'Inc', hal / 'Inc/Legacy',
  cube / 'Drivers/CMSIS/Include',
  cube / 'Drivers/CMSIS/Device/ST/STM32H5xx/Include',
)

# Keep this list aligned with CubeMX's enabled peripherals after regeneration.
hal_sources = []
foreach component : ['cortex', 'icache', 'rcc', 'rcc_ex', 'flash', 'flash_ex',
                     'gpio', 'dma', 'dma_ex', 'pwr', 'pwr_ex', 'exti', 'uart', 'uart_ex']
  hal_sources += files(hal / 'Src/stm32h5xx_hal_' + component + '.c')
endforeach
hal_sources += files(hal / 'Src/stm32h5xx_hal.c')

linker_script = files('STM32H563xx_FLASH.ld')
firmware = executable('stm32h563-blinky',
  'Core/Src/main.c', 'Core/Src/stm32h5xx_it.c',
  'Core/Src/stm32h5xx_hal_msp.c', 'Core/Src/system_stm32h5xx.c',
  'Core/Src/sysmem.c', 'Core/Src/syscalls.c', 'startup_stm32h563xx.s',
  hal_sources,
  'blinky.cc',
  dependencies: stm32h5_dep,
  name_suffix: 'elf',
  # libstdc++ assertion reporting pulls in allocating stdio on this toolchain.
  override_options: ['b_ndebug=true'],
  include_directories: firmware_includes,
  c_args: ['-DUSE_HAL_DRIVER', '-DSTM32H563xx'],
  # ST's device header includes math.h, which GCC 15 rejects in freestanding C++.
  # Allow those headers; the core compile check remains freestanding.
  cpp_args: ['-DUSE_HAL_DRIVER', '-DSTM32H563xx', '-D_GLIBCXX_NO_ASSERTIONS',
             '-fno-freestanding', '-fno-rtti', '-fstack-usage'],
  link_args: ['-T' + meson.current_source_dir() / 'STM32H563xx_FLASH.ld',
              '-Wl,--gc-sections', '-Wl,--print-memory-usage',
              '-Wl,-Map=' + meson.current_build_dir() / 'stm32h563-blinky.map'],
  link_depends: linker_script,
)
objcopy = find_program('objcopy')
foreach output_format : [['hex', 'ihex'], ['bin', 'binary']]
  custom_target('stm32h563-blinky-' + output_format[0],
    input: firmware, output: 'stm32h563-blinky.' + output_format[0],
    command: [objcopy, '-O', output_format[1], '@INPUT@', '@OUTPUT@'],
    build_by_default: true,
  )
endforeach
