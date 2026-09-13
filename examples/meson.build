if platform == 'stm32h5'
  subdir('stm32h563_blinky')
else
  example_platforms = {'fake': fake_dep}
  if platform == 'host'
    example_platforms += {'host': host_dep}
  endif
  subdir('hello')
  subdir('system')
endif
