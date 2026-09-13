subdir('cmsis')
if get_option('examples')
  subdir('hal')
endif

stm32h5_lib = static_library('daveos-stm32h5', 'platform.cpp',
  dependencies: [core_dep, stm32h5_cmsis_dep])
stm32h5_dep = declare_dependency(link_with: stm32h5_lib,
  dependencies: [core_dep, stm32h5_cmsis_dep])
