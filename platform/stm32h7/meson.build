subdir('cmsis')
if get_option('examples') or get_option('stm32_support')
  subdir('hal')
endif

stm32h7_lib = static_library('daveos-stm32h7', 'platform.cpp', 'reliability.cpp', 'flash.cpp', 'crc32.cpp',
  dependencies: [core_dep, util_dep, stm32h7_cmsis_dep],
  cpp_args: ['-DCORE_CM7', '-fno-freestanding'])
stm32h7_dep = declare_dependency(link_with: stm32h7_lib,
  dependencies: [core_dep, util_dep, stm32h7_cmsis_dep])
