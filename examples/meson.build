if platform == 'stm32h5'
  subdir('stm32h563_blinky')
elif platform == 'stm32h7'
  subdir('stm32h755_console')
else
  example_platforms = {'fake': fake_dep}
  if platform == 'host'
    example_platforms += {'host': host_dep}
    subdir('console')
  endif
  subdir('hello')
  subdir('system')
endif
