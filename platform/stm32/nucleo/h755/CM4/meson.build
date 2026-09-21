firmware_deps = [stm32h7_hal_dep]
foreach component : ['cortex', 'hsem', 'rcc', 'rcc_ex', 'flash', 'flash_ex',
                     'gpio', 'dma', 'dma_ex', 'pwr', 'pwr_ex', 'exti']
  firmware_deps += stm32h7_hal_components[component]
endforeach
# Override the M7 project flags for every source and for the M4 link.
cm4_cpu = ['-mcpu=cortex-m4', '-mthumb', '-mfpu=fpv4-sp-d16', '-mfloat-abi=hard']
linker_script = files('stm32h755xx_flash_CM4.ld')
firmware = executable('stm32h755-sleep-m4',
  'Core/Src/main.c', 'Core/Src/stm32h7xx_it.c',
  'Core/Src/stm32h7xx_hal_msp.c', 'Core/Src/sysmem.c', 'Core/Src/syscalls.c',
  '../Common/Src/system_stm32h7xx_dualcore_boot_cm4_cm7.c',
  'Core/Startup/startup_stm32h755xx_CM4.s',
  dependencies: firmware_deps, name_suffix: 'elf',
  include_directories: include_directories('Core/Inc'),
  link_language: 'c',
  c_args: cm4_cpu + ['-DCORE_CM4', '-DUSE_PWR_DIRECT_SMPS_SUPPLY', '-DUSER_VECT_TAB_ADDRESS'],
  link_args: cm4_cpu + ['--specs=nosys.specs', '--specs=nano.specs', '-T' + meson.current_source_dir() / 'stm32h755xx_flash_CM4.ld',
              '-Wl,--gc-sections', '-Wl,--print-memory-usage',
              '-Wl,-Map=' + meson.current_build_dir() / 'stm32h755-sleep-m4.map'],
  link_depends: linker_script,
)
foreach output_format : [['hex', 'ihex'], ['bin', 'binary']]
  converted_m4 = custom_target('stm32h755-sleep-m4-' + output_format[0],
    input: firmware, output: 'stm32h755-sleep-m4.' + output_format[0],
    command: [objcopy, '-O', output_format[1], '@INPUT@', '@OUTPUT@'],
    build_by_default: true)
  if output_format[0] == 'bin'
    m4_binary = converted_m4
  endif
endforeach

if not get_option('bootloader')
  firmware_images += [firmware]
endif
