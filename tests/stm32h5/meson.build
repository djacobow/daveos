exe = executable('test-stm32h5', '../stm32h5.cc', '../../src/platform/stm32h5/platform.cc',
  include_directories: include_directories('.'), dependencies: [core_dep, catch_dep])
test('stm32h5', exe, suite: 'unit', timeout: 20)
