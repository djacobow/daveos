exe = executable('test-usb-console', 'usb_console.cpp',
  '../../platform/stm32/console/usb/usb.cpp',
  dependencies: [test_support_dep, console_dep],
  include_directories: include_directories('.'))
test('usb-console', exe, suite: 'unit', timeout: 20)
