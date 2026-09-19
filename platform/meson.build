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
  subdir('fake')
  if platform == 'host'
    subdir('host')
  endif
endif
