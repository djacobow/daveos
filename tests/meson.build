subdir('catch2')
subdir('core')
subdir('timers')
subdir('logging')
subdir('stm32h5')
# TSan owns allocator interceptors; other configurations test allocation.
if not get_option('b_sanitize').contains('thread')
  subdir('allocation')
endif
if platform == 'host'
  subdir('host')
endif
