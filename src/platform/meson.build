if platform == 'stm32h5'
  subdir('stm32h5')
else
  threads = dependency('threads')
  subdir('fake')
  if platform == 'host'
    subdir('host')
  endif
endif
