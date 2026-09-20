if platform in ['stm32h5', 'stm32h7']
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
  subdir('fake')
  if platform == 'host'
    subdir('host')
  endif
endif
