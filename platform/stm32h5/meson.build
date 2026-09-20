subdir('cmsis')
if get_option('examples') or get_option('stm32_support')
  subdir('hal')
endif

stm32h5_lib = static_library('daveos-stm32h5', 'platform.cpp', 'crc32.cpp', 'reliability.cpp', 'otp.cpp',
  dependencies: [core_dep, stm32h5_cmsis_dep, otp_dep])
stm32h5_dep = declare_dependency(link_with: stm32h5_lib,
  dependencies: [core_dep, stm32h5_cmsis_dep, otp_dep])
