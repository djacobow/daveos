stm32h5_lib = static_library('daveos-stm32h5', 'platform.cc',
  dependencies: [core_dep, stm32h5_cmsis_dep])
stm32h5_dep = declare_dependency(link_with: stm32h5_lib,
  dependencies: [core_dep, stm32h5_cmsis_dep])
