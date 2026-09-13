if platform in ['stm32h5', 'stm32h7']
  subdir(platform)
else
  threads = dependency('threads')
  subdir('fake')
  if platform == 'host'
    subdir('host')
  endif
endif
