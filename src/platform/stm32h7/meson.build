stm32h7_lib = static_library('daveos-stm32h7', 'platform.cc',
  dependencies: [core_dep, stm32h7_cmsis_dep],
  cpp_args: ['-DCORE_CM7', '-fno-freestanding'])
stm32h7_dep = declare_dependency(link_with: stm32h7_lib,
  dependencies: [core_dep, stm32h7_cmsis_dep])
