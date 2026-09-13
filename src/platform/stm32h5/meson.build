cube = '../../../platform/stm32h5/STM32CubeH5'
stm32h5_lib = static_library('daveos-stm32h5', 'platform.cc',
  dependencies: core_dep,
  include_directories: include_directories(
    cube / 'Drivers/CMSIS/Include',
    cube / 'Drivers/CMSIS/Device/ST/STM32H5xx/Include'),
  cpp_args: ['-DSTM32H563xx'],
)
stm32h5_dep = declare_dependency(link_with: stm32h5_lib, dependencies: core_dep)
