# Match the firmware's C++ library mode: GCC 13's <chrono> requires hosted
# headers. This still cross-compiles against newlib without host/OS APIs;
# allowing these headers does not imply allocating or using hosted facilities.
static_library('daveos-arm-core-check', 'arm_compile.cpp', dependencies: core_dep,
  cpp_args: ['-fno-freestanding'])
if platform == 'stm32h5'
  bus_cmsis = stm32h5_cmsis_dep
  bus_defines = []
else
  bus_cmsis = stm32h7_cmsis_dep
  bus_defines = ['-DCORE_CM7']
endif
static_library('daveos-arm-bus-check', 'bus.cpp', dependencies: [hal_dep, bus_cmsis],
  cpp_args: bus_defines + ['-fno-freestanding'])
