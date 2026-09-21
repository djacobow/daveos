if platform in ['stm32h5', 'stm32h7']
  subdir('stm32/memory')
  if (get_option('examples') or get_option('stm32_support')) and get_option('usb_console')
    subdir('stm32')
  endif
  if get_option('networking')
    subdir('stm32/ethernet')
  endif
  subdir(platform)
  if get_option('examples') or get_option('stm32_support')
    subdir('stm32/console')
    subdir('stm32/nucleo')
  endif
else
  threads = dependency('threads')
  # File storage works with either the real host clock or an injected fake clock.
  file_flash_lib = static_library('daveos-file-flash', 'host/flash.cpp', dependencies: core_dep)
  file_flash_dep = declare_dependency(link_with: file_flash_lib, dependencies: core_dep)
  meson.override_dependency('daveos-file-flash', file_flash_dep)
  file_otp_lib = static_library('daveos-file-otp', 'host/otp.cpp', dependencies: otp_dep)
  file_otp_dep = declare_dependency(link_with: file_otp_lib, dependencies: otp_dep)
  meson.override_dependency('daveos-file-otp', file_otp_dep)
  if get_option('fatfs')
    file_block_lib = static_library('daveos-file-block', 'host/block_device.cpp', dependencies: core_dep)
    file_block_dep = declare_dependency(link_with: file_block_lib, dependencies: core_dep)
    meson.override_dependency('daveos-file-block', file_block_dep)
  endif
  subdir('fake')
  if platform == 'host'
    subdir('host')
  endif
endif
