subdir('catch2')
# Shared fixtures remain in tests/support.h.
test_support_dep = declare_dependency(include_directories: include_directories('.'),
  dependencies: [fake_dep, catch_dep])
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
