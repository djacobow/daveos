firmware_deps = [stm32h7_dep, stm32h7_hal_dep]
foreach component : ['cortex', 'hsem', 'rcc', 'rcc_ex', 'flash', 'flash_ex',
                     'gpio', 'dma', 'dma_ex', 'pwr', 'pwr_ex', 'exti', 'uart', 'uart_ex']
  firmware_deps += stm32h7_hal_components[component]
endforeach
linker_script = files('stm32h755xx_flash_CM7.ld')
firmware = executable('stm32h755-console-m7',
  'Core/Src/main.c', 'Core/Src/stm32h7xx_it.c',
  'Core/Src/stm32h7xx_hal_msp.c', 'Core/Src/sysmem.c', 'Core/Src/syscalls.c',
  '../Common/Src/system_stm32h7xx_dualcore_boot_cm4_cm7.c',
  'Core/Startup/startup_stm32h755xx_CM7.s', 'console.cc',
  dependencies: firmware_deps, name_suffix: 'elf',
  override_options: ['b_ndebug=true'],
  include_directories: include_directories('Core/Inc'),
  c_args: ['-DCORE_CM7', '-DUSE_PWR_DIRECT_SMPS_SUPPLY', '-DUSER_VECT_TAB_ADDRESS'],
  cpp_args: ['-DCORE_CM7', '-DUSE_PWR_DIRECT_SMPS_SUPPLY', '-DUSER_VECT_TAB_ADDRESS',
             '-D_GLIBCXX_NO_ASSERTIONS', '-fno-freestanding', '-fno-rtti', '-fstack-usage'],
  link_args: ['-T' + meson.current_source_dir() / 'stm32h755xx_flash_CM7.ld',
              '-Wl,--gc-sections', '-Wl,--print-memory-usage',
              '-Wl,-Map=' + meson.current_build_dir() / 'stm32h755-console-m7.map'],
  link_depends: linker_script,
)
foreach output_format : [['hex', 'ihex'], ['bin', 'binary']]
  custom_target('stm32h755-console-m7-' + output_format[0],
    input: firmware, output: 'stm32h755-console-m7.' + output_format[0],
    command: [objcopy, '-O', output_format[1], '@INPUT@', '@OUTPUT@'],
    build_by_default: true)
endforeach

firmware_images += [firmware]
