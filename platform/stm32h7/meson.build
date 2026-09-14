subdir('cmsis')
if get_option('examples')
  subdir('hal')
  if get_option('usb_console')
    subdir('usb_device')
  endif
endif

stm32h7_lib = static_library('daveos-stm32h7', 'platform.cpp',
  dependencies: [core_dep, stm32h7_cmsis_dep],
  cpp_args: ['-DCORE_CM7', '-fno-freestanding'])
stm32h7_dep = declare_dependency(link_with: stm32h7_lib,
  dependencies: [core_dep, stm32h7_cmsis_dep])
