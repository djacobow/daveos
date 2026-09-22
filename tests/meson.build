subdir('catch2')
# Shared fixtures remain in tests/support.hpp.
test_support_dep = declare_dependency(include_directories: include_directories('.'),
  dependencies: [fake_dep, catch_dep])
subdir('core')
subdir('consumers')
subdir('instances')
subdir('health')
subdir('lifecycle')
subdir('yield')
subdir('hal')
subdir('mcp3425')
subdir('ssd1306')
subdir('sd_inspect')
subdir('state_machine')
subdir('reliability')
subdir('otp')
subdir('events')
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
if 'net' in features
  subdir('network')
endif

subdir('usability')
subdir('application')
subdir('console_library')
if platform == 'host' and get_option('b_sanitize') == 'none'
  subdir('starter')
endif

if 'fatfs' in features
  subdir('storage')
endif
