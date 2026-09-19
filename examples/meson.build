if platform in ['stm32h5', 'stm32h7']
  subdir('stm32_console')
else
  example_platforms = {'fake': fake_dep}
  if platform == 'host'
    example_platforms += {'host': host_dep}
    subdir('console')
  endif
  subdir('hello')
  subdir('system')
endif
