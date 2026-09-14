if platform in ['stm32h5', 'stm32h7']
  if get_option('examples') and get_option('usb_console')
    subdir('stm32')
  endif
  subdir(platform)
else
  threads = dependency('threads')
  subdir('fake')
  if platform == 'host'
    subdir('host')
  endif
endif
