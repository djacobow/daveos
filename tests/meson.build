subdir('catch2')
# Shared fixtures remain in tests/support.hpp.
test_support_dep = declare_dependency(include_directories: include_directories('.'),
  dependencies: [fake_dep, catch_dep])
subdir('core')
subdir('commands')
subdir('console_input')
subdir('usb_console')
subdir('uart_output')
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
subdir('programming')
if get_option('networking')
  subdir('network')
endif
